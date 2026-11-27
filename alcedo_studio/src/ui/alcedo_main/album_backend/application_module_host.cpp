//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/application_module_host.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDebug>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "app/ai_denoise_history_write.hpp"
#include "app/ai_model_package_install.hpp"
#include "app/editor_adjustment_context.hpp"
#include "app/editor_pending_input.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/image_pool_service.hpp"
#include "app/model_asset_catalog.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/input/raw_sensor_layout.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/lens/lens_calibration_resolver.hpp"
#include "io/image/embedded_preview_reader.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/comparison_image_provider.hpp"
#include "ui/alcedo_main/album_backend/display_profile_readout.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_history_models.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_checkpoint_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_mask_asset_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_task_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_thumbnail_port.hpp"
#include "ui/alcedo_main/album_backend/justified_layout_model.hpp"
#include "ui/alcedo_main/album_backend/library_preset_preview_source.hpp"
#include "ui/alcedo_main/album_backend/mask_thumbnail_image_provider.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/system_icon_image_provider.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_image_provider.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/logging/log.hpp"

namespace alcedo::ui {
namespace {

// Weak references to the project services that keep the DuckDB file open. The destructor
// takes them before ProjectModule is destroyed and reports every service that outlives it.
struct ProjectServiceWatch {
  std::weak_ptr<alcedo::ProjectService>       project_;
  std::weak_ptr<alcedo::Storage>              storage_;
  std::weak_ptr<alcedo::ImagePoolService>     image_pool_;
  std::weak_ptr<alcedo::PipelineMgmtService>  pipeline_;
  std::weak_ptr<alcedo::ThumbnailService>     thumbnails_;
  std::weak_ptr<alcedo::MaskThumbnailService> mask_thumbnails_;
  std::weak_ptr<alcedo::ExportService>        export_;
};

auto WatchProjectServices(const ProjectModule* project) -> ProjectServiceWatch {
  ProjectServiceWatch watch;
  if (project == nullptr) {
    return watch;
  }
  const auto& handler = project->handler();
  if (const auto& service = handler.project()) {
    watch.project_    = service;
    watch.storage_    = service->GetStorage();
    watch.image_pool_ = service->GetImagePoolService();
  }
  watch.pipeline_        = handler.pipeline_service();
  watch.thumbnails_      = handler.thumbnail_service();
  watch.mask_thumbnails_ = handler.mask_thumbnail_service();
  watch.export_          = handler.export_service();
  return watch;
}

void ReportSurvivingProjectServices(const ProjectServiceWatch& watch) {
  const auto report = [](const char* name, long use_count) {
    if (use_count > 0) {
      qCWarning(alcedo::diag::appLog).noquote()
          << "Project service outlived ProjectModule:" << name << "use_count=" << use_count;
    }
  };
  report("ProjectService", watch.project_.use_count());
  report("Storage", watch.storage_.use_count());
  report("ImagePoolService", watch.image_pool_.use_count());
  report("PipelineMgmtService", watch.pipeline_.use_count());
  report("ThumbnailService", watch.thumbnails_.use_count());
  report("MaskThumbnailService", watch.mask_thumbnails_.use_count());
  report("ExportService", watch.export_.use_count());
}

}  // namespace

// ── ApplicationModuleHost ───────────────────────────────────────────────────

ApplicationModuleHost::ApplicationModuleHost(QObject* parent, LifecycleObserver observer,
                                             PackageServiceFactories package_services)
    : QObject(parent), lifecycle_observer_(std::move(observer)) {
  // Removes the runtime workspaces and DB snapshots that an abnormal exit or a failed removal
  // left in the project temp root. Workspaces that a live process holds stay.
  project_pack::RemoveAbandonedProjectTempFiles(project_pack::ProjectTempRoot());
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();
  alcedo::ui::RegisterEditorAdjustmentQmlTypes();
  alcedo::ui::RegisterJustifiedLayoutQmlType();
  alcedo::ui::RegisterEditorHistoryQmlTypes();
  alcedo::ui::RegisterEditorNodeQmlTypes();
  alcedo::ui::RegisterDisplayProfileReadoutQmlType();
  background_tasks_ = std::make_unique<BackgroundTaskController>();
  RecordConstruction("BackgroundTaskController", background_tasks_.get());
  interaction_policy_ =
      std::make_unique<InteractionPolicyController>(background_tasks_.get(), this);
  RecordConstruction("InteractionPolicyController", interaction_policy_.get());
  agent_control_ = std::make_unique<AgentControlOwner>(background_tasks_.get(), this);
  RecordConstruction("AgentControlOwner", agent_control_.get());
  download_service_ = std::make_unique<alcedo::DownloadService>();
  RecordConstruction("DownloadService", download_service_.get());
  model_download_service_ =
      std::make_unique<alcedo::ModelDownloadService>(*download_service_);
  RecordConstruction("ModelDownloadService", model_download_service_.get());
  updates_ = std::make_unique<alcedo::UpdateService>(*download_service_, this);
  RecordConstruction("UpdateService", updates_.get());
  // Loads the persisted LUT inventory without network access (plan 4.3).
  lut_library_ = std::make_unique<alcedo::LutLibraryService>(
      package_services.library_options ? package_services.library_options()
                                       : alcedo::LutLibraryServiceOptions{});
  RecordConstruction("LutLibraryService", lut_library_.get());
  lut_library_->Start();
  // Constructing the package service makes no network request; Settings starts
  // the signed feed check (plan L3). Archives use the shared download admission.
  {
    alcedo::LutPackageServiceOptions lut_package_options =
        package_services.package_options
            ? package_services.package_options()
            : alcedo::LutPackageServiceOptions::FromBuildConfiguration();
    if (!lut_package_options.downloader) {
      lut_package_options.downloader =
          std::make_unique<alcedo::DownloadServicePackageArchiveDownloader>(*download_service_);
    }
    lut_packages_ =
        std::make_unique<alcedo::LutPackageService>(std::move(lut_package_options), *lut_library_);
  }
  RecordConstruction("LutPackageService", lut_packages_.get());
  // Scans the preset folder on its worker (edit presets plan, PS1). A saved preset refers to a
  // LUT of the LUT library by its library reference (plan 3.3).
  {
    alcedo::PresetLibraryServiceOptions preset_options =
        package_services.preset_library_options ? package_services.preset_library_options()
                                                : alcedo::PresetLibraryServiceOptions{};
    if (!preset_options.lut_reference_for_path) {
      preset_options.lut_reference_for_path = [library = lut_library_.get()](
                                                  const std::filesystem::path& path) {
        return library->ReferenceForPath(path);
      };
    }
    preset_library_ = std::make_unique<alcedo::PresetLibraryService>(std::move(preset_options));
  }
  RecordConstruction("PresetLibraryService", preset_library_.get());
  preset_library_->Start();
  // Reads the DCP folder setting and starts the first scan on its worker (DCP plan C2).
  camera_profiles_ = std::make_unique<alcedo::CameraProfileLibraryService>();
  RecordConstruction("CameraProfileLibraryService", camera_profiles_.get());
  camera_profiles_->Start();
  project_ = std::make_unique<ProjectModule>(this);
  RecordConstruction("ProjectModule", project_.get());
  // Every project's editor, thumbnail, and export executors resolve LUT references through
  // the library (plan L4).
  project_->SetLutResources(lut_library_->Resources());
  // AI Denoise reads its FP16 graph from the AI Denoise package of the AI model folder of the
  // settings (alcedo-ai-denoise-v<N>, integration plan DN12), found again at each check of the
  // installed model. The session of the executors is reached through PipelineExecutor: on
  // Windows the executors are in a DLL with their own copy.
  alcedo::PipelineExecutor::ProcessAiDenoiseSession()->SetGraphLocator([] {
    alcedo::AiDenoiseGraphLocation location = alcedo::ResolveAiDenoiseGraph(
        std::filesystem::path(alcedo::ConfiguredSemanticModelDirectory().toStdWString()));
    return alcedo::AiDenoiseGraphLookup{std::move(location.graph), std::move(location.problem)};
  });
  library_ = std::make_unique<LibraryModule>(project_.get(), this);
  RecordConstruction("LibraryModule", library_.get());
  folders_ =
      std::make_unique<FolderController>(project_.get(), library_.get(), project_.get(), this);
  RecordConstruction("FolderController", folders_.get());
  images_ = std::make_unique<ImageController>(project_.get(), library_.get(), folders_.get(),
                                              project_.get(), this);
  RecordConstruction("ImageController", images_.get());
  stats_ = std::make_unique<StatsEngine>(project_.get(), library_.get(), folders_.get(), this);
  RecordConstruction("StatsEngine", stats_.get());
  search_ = std::make_unique<SearchController>(project_.get(), library_.get(), folders_.get(),
                                               stats_.get(), this);
  RecordConstruction("SearchController", search_.get());
  model_download_ = std::make_unique<ModelDownloadController>(*model_download_service_,
                                                              background_tasks_.get(), this);
  RecordConstruction("ModelDownloadController", model_download_.get());
  // Constructing the AI model package service reads the local receipts only; Settings starts
  // the signed feed check (AI Models plan, P6). Archives use the shared download admission.
  {
    alcedo::AiModelPackageServiceOptions ai_model_options =
        package_services.ai_model_package_options
            ? package_services.ai_model_package_options()
            : alcedo::AiModelPackageServiceOptions::FromBuildConfiguration();
    if (!ai_model_options.downloader) {
      ai_model_options.downloader =
          std::make_unique<alcedo::DownloadServicePackageArchiveDownloader>(*download_service_);
    }
    if (!ai_model_options.model_directory) {
      ai_model_options.model_directory = [this] {
        return std::filesystem::path(model_download_->ModelDownloadDirectory().toStdU16String());
      };
    }
    ai_model_packages_ =
        std::make_unique<alcedo::AiModelPackageService>(std::move(ai_model_options));
  }
  RecordConstruction("AiModelPackageService", ai_model_packages_.get());
  // One download statement covers the CLIP models and the AI Mask model set (plan P8).
  model_download_->SetDownloadStatementCheck(
      [packages = ai_model_packages_.get()] { return packages->download_statement_accepted(); });
  ai_model_download_tasks_ =
      std::make_unique<AiModelDownloadTaskPublisher>(*ai_model_packages_, *background_tasks_);
  RecordConstruction("AiModelDownloadTaskPublisher", ai_model_download_tasks_.get());
  // One directory setting serves all AI models (P7); a new directory has other installations.
  QObject::connect(model_download_.get(), &ModelDownloadController::StateChanged,
                   ai_model_packages_.get(),
                   [packages = ai_model_packages_.get(), downloads = model_download_.get(),
                    directory = model_download_->ModelDownloadDirectory()]() mutable {
                     if (downloads->ModelDownloadDirectory() == directory) return;
                     directory = downloads->ModelDownloadDirectory();
                     packages->RefreshInstallations();
                   });
  ai_provider_profiles_ = std::make_unique<alcedo::AiProviderProfileController>(this);
  RecordConstruction("AiProviderProfileController", ai_provider_profiles_.get());
  semantic_generation_ = std::make_unique<SemanticGenerationController>(
      project_.get(), library_.get(), model_download_.get(), background_tasks_.get(),
      project_.get(), this);
  RecordConstruction("SemanticGenerationController", semantic_generation_.get());
  import_export_ =
      std::make_unique<ImportExportHandler>(project_.get(), library_.get(), folders_.get(),
                                            project_.get(), this);
  RecordConstruction("ImportExportHandler", import_export_.get());
  import_export_->SetInteractionPolicy(interaction_policy_.get());
  export_queue_ = std::make_unique<ExportQueue>(project_.get(), import_export_.get(), this);
  RecordConstruction("ExportQueue", export_queue_.get());
  export_recipe_ = std::make_unique<ExportRecipeBuilder>(import_export_.get(), this);
  RecordConstruction("ExportRecipeBuilder", export_recipe_.get());
  export_recipe_->SetInteractionPolicy(interaction_policy_.get());
  nikon_he_recovery_ = std::make_unique<NikonHeRecoveryController>(project_.get(), images_.get(),
                                                                   project_.get(), this);
  RecordConstruction("NikonHeRecoveryController", nikon_he_recovery_.get());
  adjustment_transfer_ = std::make_unique<AdjustmentTransferController>(
      project_.get(), library_.get(), import_export_.get(), background_tasks_.get(), this);
  RecordConstruction("AdjustmentTransferController", adjustment_transfer_.get());
  adjustment_transfer_->SetInteractionPolicy(interaction_policy_.get());
  presets_ = std::make_unique<PresetController>(preset_library_.get(),
                                                adjustment_transfer_->dialog_model());
  RecordConstruction("PresetController", presets_.get());
  // Apply Preset to library photos takes the multi-target Paste (edit presets plan, PS4).
  presets_->SetAdjustmentTransfer(adjustment_transfer_.get());
  // Phase 5B/5E: wire runtime first-frame ports, direct presentation, and
  // BackgroundTaskController-backed editor_save registration into the session
  // runtime. The session scheduler accepts intents without completing when
  // no real image path / test producer is available (shell-compatible Loading).
  {
    auto session_pipeline  = std::make_shared<EditorSessionPipelinePort>();
    auto session_history   = std::make_shared<EditorSessionHistoryPort>();
    auto session_tasks     = std::make_shared<EditorSessionTaskPort>(background_tasks_.get());
    auto session_scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
    session_scheduler->SetPipelinePort(session_pipeline);

    EditorSessionPipelineMappers pipeline_services;
    pipeline_services.pipeline_service = [this]() -> std::shared_ptr<alcedo::PipelineMgmtService> {
      return project_ ? project_->handler().pipeline_service() : nullptr;
    };
    std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool =
        [this]() -> std::shared_ptr<alcedo::ImagePoolService> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      return project_->handler().project()->GetImagePoolService();
    };
    std::function<std::shared_ptr<alcedo::Storage>()> storage_service =
        [this]() -> std::shared_ptr<alcedo::Storage> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      return project_->handler().project()->GetStorage();
    };
    std::function<std::filesystem::path(sl_element_id_t)> mini_git_journal_path =
        [this](sl_element_id_t element_id) {
          if (!project_) {
            return std::filesystem::path{};
          }
          const auto directory = project_->handler().editor_journal_directory();
          if (directory.empty()) {
            return std::filesystem::path{};
          }
          return directory / ("image-" + std::to_string(static_cast<std::uint64_t>(element_id)) +
                              ".mini-git.wal");
        };
    auto refresh_focused_thumbnail =
        [this, library = QPointer<LibraryModule>(library_.get())](sl_element_id_t element_id) {
          if (!library) {
            return;
          }
          QMetaObject::invokeMethod(
              library,
              [this, library, element_id] {
                if (!library || library->project() == nullptr) {
                  return;
                }
                // The host owns the library, so it is alive while this queued call runs.
                library->NoteEditHistoryChanged(editor_session_ && editor_session_->active());
                if (auto thumbnails = library->project()->handler().thumbnail_service()) {
                  thumbnails->InvalidateThumbnail(element_id);
                }
                const auto* item = library->FindAlbumItem(element_id);
                if (item != nullptr) {
                  (void)library->thumbs().RefreshCurrentThumbnail(element_id, item->image_id);
                }
              },
              Qt::QueuedConnection);
        };
    session_pipeline->SetServices(std::move(pipeline_services));
    session_history->SetServices(EditorSessionHistoryPort::Services{mini_git_journal_path});
    session_history->SetPipelinePort(session_pipeline);
    session_scheduler->SetServices(EditorSessionSchedulerServices{image_pool});
    // One project-owned global save lock for capture → materialize → terminal
    // callback. Shared by EditorSaveCheckpointService and Mini-Git materializer.
    auto save_coordinator   = std::make_shared<alcedo::EditorSaveCheckpointCoordinator>();
    auto session_checkpoint = std::make_shared<EditorSessionCheckpointStore>();
    session_checkpoint->SetServices(EditorSessionCheckpointStore::Services{
        storage_service, mini_git_journal_path, save_coordinator});
    auto session_thumbnail =
        std::make_shared<EditorSessionThumbnailPort>(std::move(refresh_focused_thumbnail));

    // The session owner runs on a dedicated worker so parameter reduction,
    // pacing deadlines, and serial frame consumption never wait on the GUI
    // thread's event loop or window-update waits.
    // The scheduler port is also the comparison's image port: comparison pairs render on the
    // editor's own executor and worker. It is also the coverage port of the AI Mask preview, on
    // the same executor and device as the frames. ShutdownModules shuts it down (waiting for an
    // image job or a coverage job) before the session service is destroyed.
    // AI Mask R8 assets live in the project database of the open project.
    auto session_mask_assets = std::make_shared<EditorSessionMaskAssetPort>(
        [this]() -> std::shared_ptr<alcedo::PipelineMgmtService> {
          return project_ ? project_->handler().pipeline_service() : nullptr;
        });
    editor_session_runtime_ = alcedo::EditorSessionRuntime::CreateWithPorts(
        session_pipeline, session_history, session_tasks, session_scheduler, session_checkpoint,
        session_thumbnail, save_coordinator,
        std::make_shared<alcedo::EditorSessionThreadedCommandExecutor>(), session_scheduler,
        std::move(session_mask_assets), session_scheduler);
    // Completion is forward: coordinator installs on_complete at Schedule.
    editor_session_scheduler_ = std::move(session_scheduler);
  }
  RecordConstruction("EditorSessionService", editor_session_runtime_->service.get());
  RecordConstruction("EditorRenderCoordinator", editor_session_runtime_->coordinator.get());
  if (editor_session_runtime_ && editor_session_runtime_->save_coordinator) {
    RecordConstruction("EditorSaveCheckpointCoordinator",
                       editor_session_runtime_->save_coordinator.get());
  }
  editor_session_ =
      std::make_unique<EditorSessionController>(editor_session_runtime_->service.get(), this);
  RecordConstruction("EditorSessionController", editor_session_.get());
  editor_comparison_ = std::make_unique<EditorComparisonController>(
      editor_session_.get(), SharedComparisonImageStore(), this);
  RecordConstruction("EditorComparisonController", editor_comparison_.get());
  EditorAiMaskTool::Services ai_mask_services;
  ai_mask_services.runtime = [this]() -> std::shared_ptr<alcedo::AiSidecarRuntimeService> {
    if (!project_ || !project_->handler().project()) {
      return nullptr;
    }
    return project_->handler().project()->GetAiSidecarRuntimeService();
  };
  ai_mask_services.runtime_options = [this]() {
    return semantic_generation_->RuntimeOptionsForCurrentSidecarSnapshot(false);
  };
  // AI Models plan, AM5: the installed AI Mask model set of a supported version,
  // <model directory>/alcedo-ai-mask-v<version>, read again at each Open.
  ai_mask_services.model_set = [packages = ai_model_packages_.get()] {
    return packages->ReadAiMaskModelSet();
  };
  ai_mask_tool_ =
      std::make_unique<EditorAiMaskTool>(editor_session_.get(), std::move(ai_mask_services), this);
  RecordConstruction("EditorAiMaskTool", ai_mask_tool_.get());
  // AI Denoise integration plan, DN7: the switch of the RAW Decode card.
  EditorAiDenoiseControl::Services ai_denoise_services;
  ai_denoise_services.platform_available = [] {
    return alcedo::PipelineExecutor::ProcessAiDenoiseSession()->IsPlatformAvailable();
  };
  ai_denoise_services.installed_model_version = [] {
    return alcedo::PipelineExecutor::ProcessAiDenoiseSession()->InstalledModelVersion();
  };
  const auto image_path = [this](std::uint32_t image_id) -> std::filesystem::path {
    if (project_ == nullptr || project_->handler().project() == nullptr) {
      return {};
    }
    auto pool = project_->handler().project()->GetImagePoolService();
    if (!pool) {
      return {};
    }
    try {
      return pool->Read<std::filesystem::path>(
          image_id, [](const std::shared_ptr<alcedo::Image>& image) {
            return image ? std::filesystem::path(image->image_path_) : std::filesystem::path{};
          });
    } catch (const std::exception&) {
      return {};
    }
  };
  ai_denoise_services.image_path         = image_path;
  ai_denoise_services.read_sensor_layout = &alcedo::ReadRawSensorLayout;
  ai_denoise_ = std::make_unique<EditorAiDenoiseControl>(std::move(ai_denoise_services),
                                                         editor_session_.get(), this);
  RecordConstruction("EditorAiDenoiseControl", ai_denoise_.get());
  // AI Denoise integration plan, DN8: Turn Off AI Denoise of an export queue item. Several photos
  // get the switch through Copy and Paste Adjustments (the RAW Decode item).
  ExportAiDenoiseControl::Services export_denoise_services;
  export_denoise_services.user_refusal = [this] {
    return UserStateChangeRefusal(interaction_policy_.get());
  };
  export_denoise_services.stored_history_writer =
      [this]() -> ExportAiDenoiseControl::StoredHistoryWriter {
    auto pipeline = project_ != nullptr ? project_->handler().pipeline_service() : nullptr;
    if (!pipeline) {
      return {};
    }
    return [pipeline = std::move(pipeline)](
               sl_element_id_t element_id, const std::optional<alcedo::DevelopAiDenoise>& denoise) {
      return alcedo::WriteAiDenoiseSwitchToStoredHistory(*pipeline, element_id, denoise);
    };
  };
  export_denoise_services.editor_element_id = [session = editor_session_.get()] {
    return session->has_image() ? static_cast<sl_element_id_t>(session->element_id())
                                : sl_element_id_t{0};
  };
  export_denoise_services.turn_off_editor_photo = [control = ai_denoise_.get()] {
    return control->turnOff();
  };
  export_denoise_services.refresh_written_photo =
      [this](sl_element_id_t element_id, const alcedo::AiDenoiseHistoryWrite& write) {
        const auto* item     = library_->FindAlbumItem(element_id);
        const auto  image_id = item != nullptr ? item->image_id : image_id_t{0};
        library_->SetAlbumItemEditState(element_id, 0, write.head_created_at_ns,
                                        QString::fromStdString(write.version_name));
        if (auto thumbnails = project_->handler().thumbnail_service()) {
          try {
            thumbnails->InvalidateThumbnail(element_id);
          } catch (const std::exception& error) {
            ALCEDO_LOG_WARN(Render)
                << "AI Denoise: thumbnail of " << element_id << ": " << error.what();
          }
        }
        if (image_id != 0 && !library_->thumbs().RefreshCurrentThumbnail(element_id, image_id)) {
          library_->thumbs().UpdateThumbnailState(element_id, QString(), false, false);
        }
        // The write changes stored edits; an edit-ordered library shows them.
        library_->NoteEditHistoryChanged(editor_session_->active());
      };
  export_ai_denoise_ = std::make_unique<ExportAiDenoiseControl>(
      std::move(export_denoise_services), background_tasks_.get(), export_queue_.get(), this);
  RecordConstruction("ExportAiDenoiseControl", export_ai_denoise_.get());
  // AI Denoise integration plan, DN14: the image of the viewport before the first frame.
  editor_placeholder_image_ = std::make_shared<EditorPlaceholderImage>([this] {
    const auto store = library_ != nullptr ? library_->thumbs().image_store() : nullptr;
    return store ? store->Destination() : nullptr;
  });
  EditorViewportPlaceholder::Services placeholder_services;
  placeholder_services.held_thumbnail = [this](sl_element_id_t element_id)
      -> std::optional<EditorViewportPlaceholder::EncodedImage> {
    const auto store = library_ != nullptr ? library_->thumbs().image_store() : nullptr;
    if (!store) {
      return std::nullopt;
    }
    for (const std::uint32_t max_edge : {2048U, 1024U, 512U, 256U}) {
      auto image = store->Get(element_id, max_edge);
      if (!image.isNull()) {
        return EditorViewportPlaceholder::EncodedImage{
            std::move(image), store->Encoding(element_id, max_edge).value_or(ViewerDisplayConfig{})};
      }
    }
    return std::nullopt;
  };
  placeholder_services.image_path             = image_path;
  placeholder_services.crop_or_rotation_check = [this](sl_element_id_t element_id)
      -> std::function<bool()> {
    auto pipeline = project_ != nullptr ? project_->handler().pipeline_service() : nullptr;
    if (!pipeline) {
      return {};
    }
    return [pipeline = std::move(pipeline), element_id] {
      const auto snapshot = pipeline->AcquireCommittedSnapshot(element_id);
      const auto& geometry = snapshot->Document().Geometry();
      return !geometry.CropRect().IsFullFrame() || geometry.RotationDegrees() != 0.0f;
    };
  };
  placeholder_services.read_camera_preview = [](const std::filesystem::path& path) -> QImage {
    // A shorter edge of at least 768 px: enough for the viewport at the fit zoom of most windows;
    // a large JPEG preview then decodes at a reduced scale.
    const auto preview = alcedo::EmbeddedPreviewReader::Read(path, 768);
    if (!preview.has_value() || preview->rgb8.empty()) {
      return {};
    }
    const auto& rgb = preview->rgb8;
    return QImage(rgb.data, rgb.cols, rgb.rows, static_cast<qsizetype>(rgb.step),
                  QImage::Format_RGB888)
        .copy();
  };
  editor_placeholder_ = std::make_unique<EditorViewportPlaceholder>(
      std::move(placeholder_services), editor_placeholder_image_, this);
  RecordConstruction("EditorViewportPlaceholder", editor_placeholder_.get());
  QObject::connect(editor_session_.get(), &EditorSessionController::StateChanged,
                   editor_placeholder_.get(),
                   [session = editor_session_.get(), placeholder = editor_placeholder_.get()] {
                     if (!session->has_image()) {
                       placeholder->SetWaitingPhoto(0U, 0U);
                       return;
                     }
                     switch (session->session_state()) {
                       case alcedo::EditorSessionState::Acquiring:
                       case alcedo::EditorSessionState::Loading:
                       case alcedo::EditorSessionState::Switching:
                         placeholder->SetWaitingPhoto(session->element_id(), session->image_id());
                         break;
                       case alcedo::EditorSessionState::Interactive:
                         // The image stays until the viewport composes the frame (QML release).
                         placeholder->SetFirstFrameReady();
                         break;
                       default:
                         placeholder->SetWaitingPhoto(0U, 0U);
                         break;
                     }
                   });
  QObject::connect(editor_session_.get(), &EditorSessionController::StateChanged, ai_denoise_.get(),
                   [session = editor_session_.get(), control = ai_denoise_.get()] {
                     control->SetImage(session->has_image() ? session->image_id() : 0U);
                   });
  // A published inventory or root can change which LUT bytes the open image renders.
  QObject::connect(lut_library_.get(), &alcedo::LutLibraryService::InventoryChanged,
                   editor_session_.get(),
                   [session = editor_session_.get()] { session->NotifyLutResourcesChanged(); });
  editor_session_->SetInteractionPolicy(interaction_policy_.get());
  editor_session_->SetAlbumCatalog(library_.get());
  // control.acquire waits while the user drags a slider: the pending input of the editor has a
  // sequence that no Release or Cancel boundary sealed. Every session result and state change
  // can end that state, so the waiting acquires are tried again on both.
  agent_control_->SetUnsealedInputReader([session = editor_session_.get()] {
    const auto pending = session->PeekPendingInput();
    return std::any_of(pending.sequences.begin(), pending.sequences.end(),
                       [](const alcedo::EditorPendingSequence& sequence) {
                         return sequence.seal == alcedo::EditorPendingInputBoundaryKind::None;
                       });
  });
  QObject::connect(editor_session_.get(), &EditorSessionController::SessionResultObserved,
                   agent_control_.get(),
                   [control = agent_control_.get()] { control->RecheckPendingAcquires(); });
  QObject::connect(editor_session_.get(), &EditorSessionController::StateChanged,
                   agent_control_.get(),
                   [control = agent_control_.get()] { control->RecheckPendingAcquires(); });
  QObject::connect(library_.get(), &LibraryModule::LastEditedFileFound, editor_session_.get(),
                   [session = editor_session_.get()](uint element_id, uint image_id) {
                     session->restoreLastEditedImage(element_id, image_id);
                   });
  // Leaving the editor shows the library again: apply edits written while it was hidden.
  QObject::connect(editor_session_.get(), &EditorSessionController::StateChanged, library_.get(),
                   [session = editor_session_.get(), library = library_.get()] {
                     if (!session->active()) {
                       library->RefreshStaleEditOrder();
                     }
                   });
  editor_session_->SetImageExifReader([this](uint image_id) -> alcedo::EditorImageExifDisplay {
    if (image_id == 0 || project_ == nullptr || project_->handler().project() == nullptr) {
      return {};
    }
    auto pool = project_->handler().project()->GetImagePoolService();
    if (!pool) {
      return {};
    }
    alcedo::EditorImageExifDisplay display;
    try {
      display = pool->Read<alcedo::EditorImageExifDisplay>(
          static_cast<std::uint32_t>(image_id), [](const std::shared_ptr<alcedo::Image>& image) {
            if (!image) {
              return alcedo::EditorImageExifDisplay{};
            }
            return alcedo::ReadEditorImageExifDisplay(*image);
          });
    } catch (const std::exception&) {
      return {};
    }
    // Auto names the profile that Lens Correction matches for the image lens, so enabling the
    // correction with Auto checked writes the lens that the render already uses. A fixed-lens
    // camera without a lens name gets its built-in lens profile (#960).
    alcedo::LensInputMeta meta;
    meta.cam_maker_         = display.camera_make;
    meta.cam_model_         = display.camera_model;
    meta.lens_maker_        = display.lens_make;
    meta.lens_model_        = display.lens_model;
    meta.focal_length_mm_   = display.focal_mm.value_or(0.0f);
    meta.aperture_f_number_ = display.aperture.value_or(0.0f);
    if (const auto profile = alcedo::LensCalibrationResolver::ShotLensProfile(
            meta, alcedo::DevelopPayload{}.lens_profile_db_path)) {
      display.auto_lens_make  = profile->maker_;
      display.auto_lens_model = profile->model_;
    }
    return display;
  });
  // The reader above runs on the UI thread; load the Lensfun database here so it does not.
  lensfun_preload_ = std::thread([] {
    alcedo::LensCalibrationResolver::PreloadDatabase(alcedo::DevelopPayload{}.lens_profile_db_path);
  });
  adjustment_transfer_->SetEditorSession(editor_session_.get());
  presets_->SetEditorSession(editor_session_.get());
  connect(adjustment_transfer_.get(), &AdjustmentTransferController::PackageChanged,
          editor_session_.get(), [this]() {
            if (editor_session_) {
              editor_session_->SetCopiedPackageAvailable(adjustment_transfer_->package_available());
            }
          });
  editor_session_->SetCopiedPackageAvailable(adjustment_transfer_->package_available());
  if (editor_session_scheduler_) {
    editor_session_scheduler_->SetSinkResolver([this]() -> alcedo::IFrameSink* {
      return editor_session_ ? editor_session_->presentation_frame_sink() : nullptr;
    });
    editor_session_scheduler_->SetMaskCoverageSinkResolver([this]() -> alcedo::IMaskCoverageSink* {
      return editor_session_ ? editor_session_->presentation_mask_coverage_sink() : nullptr;
    });
    editor_session_scheduler_->SetDetailMaskCoverageSinkResolver(
        [this]() -> alcedo::IMaskCoverageSink* {
          return editor_session_ ? editor_session_->presentation_detail_mask_coverage_sink()
                                 : nullptr;
        });
  }
  // An export and semantic generation render on their own executors. The editor releases its GPU
  // resources before either allocates, and allocates them again on its next frame after the last
  // of them ends.
  const auto editor_gpu_release_hooks = [this](EditorGpuReleaseHolder holder) {
    return EditorGpuResourceReleaseHooks{
        .release =
            [this, holder](EditorGpuResourceReleaseHooks::ReleaseCompletion on_released) {
              const auto scheduler = editor_session_scheduler_;
              if (!scheduler) {
                on_released(true, {});
                return;
              }
              // Queued on the session owner before the port refuses its first frame, so the
              // owner records every refused frame and routes it again only after the release.
              if (editor_session_runtime_ && editor_session_runtime_->service) {
                editor_session_runtime_->service->SetSuspendedForGpuRelease(true);
              }
              (void)scheduler->AcquireGpuResourceRelease(
                  holder,
                  [on_released = std::move(on_released)](EditorGpuResourceRelease release) {
                    on_released(release.success, std::move(release.error));
                  });
            },
        .resume =
            [this, holder] {
              // The port renders again before the owner routes the refused frames.
              const bool last = editor_session_scheduler_ &&
                                editor_session_scheduler_->EndGpuResourceRelease(holder);
              if (last && editor_session_runtime_ && editor_session_runtime_->service) {
                editor_session_runtime_->service->SetSuspendedForGpuRelease(false);
              }
            },
    };
  };
  import_export_->SetExportGpuResourceRelease(
      editor_gpu_release_hooks(EditorGpuReleaseHolder::Export));
  semantic_generation_->SetEditorGpuResourceRelease(
      editor_gpu_release_hooks(EditorGpuReleaseHolder::SemanticGeneration));
  lut_browser_ = std::make_unique<LutLibraryModel>();
  lut_browser_->setLibrary(lut_library_.get());
  RecordConstruction("LutLibraryModel", lut_browser_.get());
  lut_target_ = std::make_unique<LutLibraryController>();
  lut_target_->setLibrary(lut_library_.get());
  lut_target_->setEditorSession(editor_session_.get());
  RecordConstruction("LutLibraryController", lut_target_.get());
  // The browser marks the entry the target applies; the two stay separate owners.
  QObject::connect(lut_target_.get(), &LutLibraryController::associationChanged,
                   lut_browser_.get(), [browser = lut_browser_.get(), target = lut_target_.get()] {
                     browser->setAppliedEntryId(target->associationEntryId());
                   });
  lut_browser_->setAppliedEntryId(lut_target_->associationEntryId());
  lut_preview_ = std::make_unique<LutPreviewController>();
  lut_preview_->setLibrary(lut_library_.get());
  lut_preview_->setEditorSession(editor_session_.get());
  RecordConstruction("LutPreviewController", lut_preview_.get());
  preset_preview_ = std::make_unique<PresetPreviewController>();
  preset_preview_->SetLibrary(preset_library_.get());
  preset_preview_->SetEditorSession(editor_session_.get());
  RecordConstruction("PresetPreviewController", preset_preview_.get());
  // The previews of the Apply Preset dialog on its first library photo (plan PS4, step 3), on the
  // thumbnail executors of the open project. A provider of its own: the display ids of the two
  // controllers are independent.
  preset_dialog_preview_ = std::make_unique<PresetPreviewController>();
  preset_dialog_preview_->SetImageStore(SharedPresetDialogPreviewImageStore());
  preset_dialog_preview_->SetOwnedSource(std::make_unique<LibraryPresetPreviewSource>(
      preset_library_.get(),
      [project = project_.get()] { return project->handler().pipeline_service(); },
      [project = project_.get()] { return project->handler().thumbnail_service(); }));
  preset_dialog_preview_->SetLibrary(preset_library_.get());
  RecordConstruction("PresetPreviewController", preset_dialog_preview_.get());
  workspace_router_ = std::make_unique<WorkspaceRouter>(editor_session_.get(), this);
  workspace_router_->SetInteractionPolicy(interaction_policy_.get());
  RecordConstruction("WorkspaceRouter", workspace_router_.get());
  editor_behavior_ = std::make_unique<EditorBehaviorPreferences>(this);
  RecordConstruction("EditorBehaviorPreferences", editor_behavior_.get());
  library_behavior_ = std::make_unique<LibraryBehaviorPreferences>(this);
  RecordConstruction("LibraryBehaviorPreferences", library_behavior_.get());
  library_view_ = std::make_unique<LibraryViewPreferences>(this);
  RecordConstruction("LibraryViewPreferences", library_view_.get());
  // The application QSettings, which the import service of project_handler.cpp reads.
  raw_import_look_ = std::make_unique<RawImportLookSettingsModel>(QString{}, this);
  RecordConstruction("RawImportLookSettingsModel", raw_import_look_.get());
  log_folder_ = std::make_unique<LogFolderController>(this);
  RecordConstruction("LogFolderController", log_folder_.get());
  project_launch_ = std::make_unique<ProjectLaunchCoordinator>(project_.get(), this);
  RecordConstruction("ProjectLaunchCoordinator", project_launch_.get());
  project_launch_->SetInteractionPolicy(interaction_policy_.get());
  application_close_ = std::make_unique<ApplicationCloseCoordinator>(
      editor_session_.get(), workspace_router_.get(), project_.get(), this);
  RecordConstruction("ApplicationCloseCoordinator", application_close_.get());
  library_mutations_ = std::make_unique<LibraryMutationOperations>(
      images_.get(), library_->selection(), folders_.get(), editor_session_.get(),
      workspace_router_.get(), this);
  RecordConstruction("LibraryMutationOperations", library_mutations_.get());
  library_mutations_->SetInteractionPolicy(interaction_policy_.get());
  connect(library_mutations_.get(), &LibraryMutationOperations::ElementsDeleted,
          export_queue_.get(), &ExportQueue::RemoveElements);

  MountScanCoordinatorDependencies mount_scan_dependencies;
  mount_scan_dependencies.tasks           = background_tasks_.get();
  mount_scan_dependencies.clock           = CreateSteadyMountScanClock();
  mount_scan_dependencies.watcher         = CreatePlatformMountDirectoryWatcher();
  mount_scan_dependencies.on_batch_stored = [library = library_.get(),
                                             folders = folders_.get()](mount_id_t) {
    // The Library shows the photos of each stored batch in place.
    library->RequestLibraryContentRefresh();
    folders->RefreshImageCounts();
  };
  mount_scan_dependencies.on_scan_ended = [library = library_.get(), folders = folders_.get()](
                                              mount_id_t, const MountScanResult& result) {
    const bool folders_changed = result.folders_added_count + result.folders_removed_count > 0;
    if (folders_changed) {
      library->ReloadFolderTree();
    }
    if (folders_changed || MountScanChangedPhotos(result)) {
      library->RequestLibraryContentRefresh();
    }
    // The count read also reads the mount states: an offline or relocated mount changes its
    // rows with no photo change.
    folders->RefreshImageCounts();
  };
  mount_scans_ = std::make_unique<MountScanCoordinator>(std::move(mount_scan_dependencies));
  RecordConstruction("MountScanCoordinator", mount_scans_.get());
  folders_->BindMountScans(mount_scans_.get());
  application_close_->BindMountScans(mount_scans_.get());
  // #1016: a user import and a delete hold the scans for their length.
  import_export_->BindMountScans(mount_scans_.get());
  images_->BindMountScans(mount_scans_.get());
  if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    connect(gui, &QGuiApplication::applicationStateChanged, mount_scans_.get(),
            [scans = mount_scans_.get()](Qt::ApplicationState state) {
              scans->HandleApplicationActiveChanged(state == Qt::ApplicationActive);
            });
  }
  connect(project_.get(), &ProjectModule::ProjectEnteredChanged, this,
          [this] { SyncMountScans(); });
  connect(project_.get(), &ProjectModule::ProjectLoadStateChanged, this,
          [this] { SyncMountScans(); });

  library_->BindCollaborators(folders_.get(), search_.get(), stats_.get());
  library_->SetSemanticLabelProvider(
      [semantic = semantic_generation_.get()](sl_element_id_t element_id) {
        return semantic ? semantic->LabelDisplayText(element_id) : QString{};
      });
  folders_->BindCollaborators(stats_.get(), search_.get(), import_export_.get());
  images_->BindCollaborators(stats_.get(), import_export_.get(), semantic_generation_.get(),
                             interaction_policy_.get(), background_tasks_.get());
  stats_->BindCollaborators(search_.get(), semantic_generation_.get());
  semantic_generation_->BindCollaborators(nikon_he_recovery_.get());
  import_export_->BindCollaborators(stats_.get(), nikon_he_recovery_.get(),
                                    semantic_generation_.get());
  nikon_he_recovery_->BindCollaborators(import_export_.get(), semantic_generation_.get());

  ProjectLifecycleHooks lifecycle_hooks;
  lifecycle_hooks.project_switch_block_reason = [import_export = import_export_.get()] {
    // ImportRunning() stays true while the finished import is written to the project.
    if (import_export && (import_export->ImportRunning() ||
                          (import_export->current_import_job() &&
                           !import_export->current_import_job()->IsCancelationAcked()))) {
      return QStringLiteral("Cannot switch project while an import is running.");
    }
    if (import_export && import_export->export_inflight()) {
      return QStringLiteral("Cannot switch project while export is running.");
    }
    return QString{};
  };
  lifecycle_hooks.finalize_editor_session = [editor_session   = editor_session_.get(),
                                             workspace_router = workspace_router_.get()] {
    // Forget the last-edited image so re-entering the editor in the next
    // project does not restore an image that belongs to the old project.
    if (editor_session) {
      editor_session->clearLastEditedImage();
    }
    if (editor_session && editor_session->active()) {
      editor_session->Finalize(true);
    }
    if (workspace_router) {
      workspace_router->OpenLibrary();
    }
  };
  lifecycle_hooks.editor_session_busy = [editor_session = editor_session_.get()] {
    return editor_session != nullptr &&
           (editor_session->active() || editor_session->has_image() ||
            editor_session->close_in_flight() || editor_session->persist_in_flight());
  };
  lifecycle_hooks.clear_project_ui_state = [library = library_.get(), folders = folders_.get(),
                                            import_export = import_export_.get()] {
    if (library) {
      library->thumbs().ReleaseVisibleThumbnailPins();
      library->view_state().all_images_.clear();
      library->view_state().total_count_ = 0;
      library->model().resetModel({}, 0);
    }
    if (folders) {
      folders->ClearState();
    }
    if (import_export) {
      import_export->ClearImportTarget();
    }
    if (library) {
      library->NotifyThumbnailsChanged();
      library->NotifyCountsChanged();
    }
    if (folders) {
      emit folders->FoldersChanged();
      emit folders->FolderSelectionChanged();
      emit folders->folderSelectionChanged();
    }
  };
  lifecycle_hooks.project_opened = [library = library_.get(), folders = folders_.get(),
                                    stats = stats_.get(), import_export = import_export_.get(),
                                    semantic = semantic_generation_.get(),
                                    editor_session = editor_session_.get(),
                                    project = project_.get()] {
    if (import_export) {
      import_export->ResetExportState();
    }
    if (library) {
      // The tree read keeps the current folder (the root after ClearState).
      library->ReloadFolderTree();
    }
    if (stats) {
      stats->ClearFilters();
    }
    if (library) {
      library->ReloadCurrentFolder();
      library->RevealLastEditedFile();
    }
    if (semantic) {
      semantic->RefreshSemanticState();
    }
    if (stats) {
      emit stats->StatsFilterChanged();
    }
    if (library) {
      library->ApplyThumbnailDiskCacheSettingsToService();
    }
    if (editor_session && project) {
      editor_session->SetMaskThumbnailService(project->handler().mask_thumbnail_service());
    }
  };
  lifecycle_hooks.should_keep_semantic_model_data =
      [model_download = model_download_.get()](const QString& profile_id) {
        return model_download && model_download->ShouldKeepSemanticModelData(profile_id);
      };
  lifecycle_hooks.refresh_semantic_state = [semantic = semantic_generation_.get()] {
    if (semantic) {
      semantic->RefreshSemanticState();
    }
  };
  lifecycle_hooks.export_inflight = [import_export = import_export_.get()] {
    return import_export && import_export->export_inflight();
  };
  lifecycle_hooks.refresh_translations = [folders = folders_.get(), library = library_.get(),
                                          stats         = stats_.get(),
                                          import_export = import_export_.get()] {
    if (folders && !folders->folder_entries().empty()) {
      folders->RebuildFolderView();
    }
    if (library && !library->model().items().empty() && stats) {
      stats->RebuildThumbnailView();
    }
    if (stats) {
      stats->RefreshStats();
    }
    if (import_export) {
      emit import_export->ImportStateChanged();
      emit import_export->importStateChanged();
      emit import_export->ExportStateChanged();
      emit import_export->exportStateChanged();
    }
  };
  lifecycle_hooks.stop_mount_scans = [scans = mount_scans_.get()] {
    if (scans) {
      scans->Stop();
    }
  };
  lifecycle_hooks.begin_stop_mount_scans = [scans = mount_scans_.get()] {
    return scans ? scans->BeginStop() : std::shared_future<void>{};
  };
  lifecycle_hooks.resume_mount_scans = [this] { SyncMountScans(); };
  project_->SetLifecycleHooks(std::move(lifecycle_hooks));
}

void ApplicationModuleHost::SyncMountScans() {
  if (!mount_scans_) {
    return;
  }
  auto&      handler    = project_->handler();
  const auto project    = handler.project();
  auto*      importer   = handler.import_service();
  const bool should_run = !shutting_down_ && project && importer != nullptr &&
                          handler.project_entered() && !handler.project_loading() &&
                          project->GetMountService();
  if (!should_run) {
    // No wait on the UI thread: the project load, the project close, and the exit each wait for
    // the scan worker before they save or replace the project.
    (void)mount_scans_->BeginStop();
    return;
  }
  if (mount_scans_->IsStarted()) {
    return;
  }
  auto invalidate_thumbnail =
      [library = QPointer<LibraryModule>(library_.get())](sl_element_id_t element_id) {
        // Called on the scan worker; the thumbnail state belongs to the UI thread.
        QMetaObject::invokeMethod(
            library,
            [library, element_id] {
              if (!library || library->project() == nullptr) {
                return;
              }
              if (auto thumbnails = library->project()->handler().thumbnail_service()) {
                thumbnails->InvalidateThumbnail(element_id);
              }
              const auto* item = library->FindAlbumItem(element_id);
              if (item != nullptr) {
                (void)library->thumbs().RefreshCurrentThumbnail(element_id, item->image_id);
              }
            },
            Qt::QueuedConnection);
      };
  // #1015: a scan that stored changes saves the project file on its worker, like the finalize
  // step of a user import.
  mount_scans_->Start(std::make_shared<ProjectMountScanPort>(project->GetMountService(), importer,
                                                             std::move(invalidate_thumbnail),
                                                             handler.MakeSaveAndPackageJob()));
}

void ApplicationModuleHost::ShutdownModules() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  try {
    // Wake blocked save-lock waiters before tearing down editor session work so
    // recovery / materialize threads can exit and join cleanly.
    if (editor_session_runtime_ && editor_session_runtime_->save_coordinator) {
      editor_session_runtime_->save_coordinator->Shutdown();
    }
    if (background_tasks_) {
      background_tasks_->CancelAll();
    }
    // The stop request returns at once. The event wait below runs until the scan worker has
    // ended (IsIdle); the project save runs after it.
    if (mount_scans_) {
      (void)mount_scans_->BeginStop();
    }
    if (semantic_generation_) {
      semantic_generation_->CancelGeneration();
    }
    // Cancel a LUT package download before the library worker stops; an
    // installation that already committed its receipt completes in Shutdown().
    if (lut_packages_) {
      lut_packages_->Shutdown();
    }
    // Cancel an AI model download and wait for its install worker (AI Models plan, AM1).
    if (ai_model_packages_) {
      ai_model_packages_->Shutdown();
    }
    if (preset_library_) {
      preset_library_->Shutdown();
    }
    if (lut_library_) {
      lut_library_->Shutdown();
    }
    if (camera_profiles_) {
      camera_profiles_->Shutdown();
    }
    if (search_) {
      search_->CancelSearchRequests();
      search_->CancelSearchPreviewThumbnails();
    }
    if (library_) {
      library_->thumbs().ReleaseVisibleThumbnailPins();
    }
    // The AI Mask tool cancels its edit and unloads the models before the editor finalizes.
    if (ai_mask_tool_) {
      ai_mask_tool_->Shutdown();
    }
    if (editor_session_ && editor_session_->active()) {
      editor_session_->Finalize(true);
    }
    if (workspace_router_) {
      workspace_router_->OpenLibrary();
    }
    if (editor_session_) {
      editor_session_->Shutdown();
    }
    // The in-flight editor frame presents to the viewport sink or the scope sink of the editor
    // session. Wait for it before either sink owner is destroyed.
    if (editor_session_scheduler_) {
      editor_session_scheduler_->Shutdown();
    }
    if (import_export_) {
      import_export_->CancelImport();
    }

    // Deliver cancellation/finalization callbacks while every module is still
    // alive. Export is a wait-for-finish task.
    const QDeadlineTimer deadline(15000);
    while (!IsIdle() && !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    // A scan worker that outlived the deadline still writes its batch. The save needs each
    // batch stored in full, so this call waits for the worker.
    if (mount_scans_) {
      mount_scans_->Stop();
    }
    // The runtime workspace is removed in the destructor, after ProjectModule closes the
    // project database (RemovePendingWorkspace).
    if (project_) {
      pending_workspace_removal_ = project_->handler().workspace_dir();
    }
    // A project that the user did not enter (welcome surface preview) has no user changes; its
    // package stays as it is on disk and only the thumbnail cache index is flushed.
    if (project_) {
      QString ignored_error;
      (void)project_->handler().PersistProjectForClose(&ignored_error);
    }
    // No render runs after the export and the editor scheduler ended; the AI Denoise session
    // ends before ONNX Runtime and DirectML unload.
    alcedo::PipelineExecutor::ProcessAiDenoiseSession()->Release();
  } catch (...) {
  }
}

ApplicationModuleHost::~ApplicationModuleHost() {
  if (lensfun_preload_.joinable()) {
    lensfun_preload_.join();
  }
  ShutdownModules();
  // The session-owner worker calls the observers that EditorSessionController installed, and a
  // callback it already copied still runs after the controller clears its observer slots. The
  // Shutdown command that ShutdownModules queued may still be running there, so join the worker
  // before any module that receives session callbacks is destroyed.
  if (editor_session_runtime_ && editor_session_runtime_->service) {
    editor_session_runtime_->service->StopCommandExecutor();
  }

  // Destroy explicitly so the lifecycle observer records the actual object
  // lifetime, including non-QObject infrastructure, and so dependent modules
  // are visibly torn down before the services they reference.
  auto destroy = [this](auto& pointer, const char* type_name) {
    const void* object = pointer.get();
    pointer.reset();
    RecordDestruction(type_name, object);
  };
  destroy(mount_scans_, "MountScanCoordinator");
  destroy(library_mutations_, "LibraryMutationOperations");
  destroy(application_close_, "ApplicationCloseCoordinator");
  destroy(project_launch_, "ProjectLaunchCoordinator");
  destroy(log_folder_, "LogFolderController");
  destroy(raw_import_look_, "RawImportLookSettingsModel");
  destroy(library_view_, "LibraryViewPreferences");
  destroy(library_behavior_, "LibraryBehaviorPreferences");
  destroy(editor_behavior_, "EditorBehaviorPreferences");
  destroy(workspace_router_, "WorkspaceRouter");
  destroy(preset_dialog_preview_, "PresetPreviewController");
  destroy(preset_preview_, "PresetPreviewController");
  destroy(lut_preview_, "LutPreviewController");
  destroy(lut_target_, "LutLibraryController");
  destroy(lut_browser_, "LutLibraryModel");
  destroy(editor_placeholder_, "EditorViewportPlaceholder");
  destroy(export_ai_denoise_, "ExportAiDenoiseControl");
  destroy(ai_denoise_, "EditorAiDenoiseControl");
  destroy(ai_mask_tool_, "EditorAiMaskTool");
  destroy(editor_comparison_, "EditorComparisonController");
  destroy(editor_session_, "EditorSessionController");
  if (editor_session_runtime_) {
    if (editor_session_runtime_->save_coordinator) {
      editor_session_runtime_->save_coordinator->Shutdown();
      RecordDestruction("EditorSaveCheckpointCoordinator",
                        editor_session_runtime_->save_coordinator.get());
    }
    RecordDestruction("EditorRenderCoordinator", editor_session_runtime_->coordinator.get());
    RecordDestruction("EditorSessionService", editor_session_runtime_->service.get());
    editor_session_runtime_.reset();
  }
  destroy(presets_, "PresetController");
  destroy(adjustment_transfer_, "AdjustmentTransferController");
  destroy(nikon_he_recovery_, "NikonHeRecoveryController");
  destroy(export_recipe_, "ExportRecipeBuilder");
  destroy(export_queue_, "ExportQueue");
  destroy(import_export_, "ImportExportHandler");
  destroy(semantic_generation_, "SemanticGenerationController");
  destroy(ai_provider_profiles_, "AiProviderProfileController");
  destroy(ai_model_download_tasks_, "AiModelDownloadTaskPublisher");
  destroy(ai_model_packages_, "AiModelPackageService");
  destroy(model_download_, "ModelDownloadController");
  destroy(search_, "SearchController");
  destroy(stats_, "StatsEngine");
  destroy(images_, "ImageController");
  destroy(folders_, "FolderController");
  destroy(library_, "LibraryModule");
  const auto project_services = WatchProjectServices(project_.get());
  destroy(project_, "ProjectModule");
  ReportSurvivingProjectServices(project_services);
  RemovePendingWorkspace();
  destroy(camera_profiles_, "CameraProfileLibraryService");
  destroy(lut_packages_, "LutPackageService");
  destroy(preset_library_, "PresetLibraryService");
  destroy(lut_library_, "LutLibraryService");
  destroy(updates_, "UpdateService");
  destroy(model_download_service_, "ModelDownloadService");
  destroy(download_service_, "DownloadService");
  destroy(agent_control_, "AgentControlOwner");
  destroy(interaction_policy_, "InteractionPolicyController");
  destroy(background_tasks_, "BackgroundTaskController");
}

void ApplicationModuleHost::Shutdown() { ShutdownModules(); }

auto ApplicationModuleHost::IsIdle() const -> bool {
  if (background_tasks_ &&
      background_tasks_->ActiveCountExcept(BackgroundTaskKind::AgentControl) > 0) {
    return false;
  }
  // An automatic scan shows no task until it changes the library, but it writes the project.
  if (mount_scans_ && mount_scans_->HasPendingScans()) {
    return false;
  }
  return !import_export_ ||
         (!import_export_->ImportRunning() && !import_export_->export_inflight());
}

void ApplicationModuleHost::RemovePendingWorkspace() {
  if (pending_workspace_removal_.empty()) {
    return;
  }
  (void)album_util::CleanupWorkspaceDirectory(pending_workspace_removal_);
  pending_workspace_removal_.clear();
}

void ApplicationModuleHost::AttachQmlEngine(QQmlEngine* engine) {
  if (engine == nullptr) {
    return;
  }

  engine->addImageProvider(QString::fromUtf8(kMaskThumbnailImageProviderId),
                           new MaskThumbnailImageProvider(SharedMaskThumbnailImageStore()));
  engine->addImageProvider(QString::fromUtf8(kSystemIconImageProviderId),
                           new SystemIconImageProvider());
  engine->addImageProvider(QString::fromUtf8(kComparisonImageProviderId),
                           new ComparisonImageProvider(SharedComparisonImageStore()));
  engine->addImageProvider(QString::fromUtf8(kLutPreviewImageProviderId),
                           new BrowserPreviewImageProvider(SharedLutPreviewImageStore()));
  engine->addImageProvider(QString::fromUtf8(kPresetPreviewImageProviderId),
                           new BrowserPreviewImageProvider(SharedPresetPreviewImageStore()));
  engine->addImageProvider(QString::fromUtf8(kPresetDialogPreviewImageProviderId),
                           new BrowserPreviewImageProvider(SharedPresetDialogPreviewImageStore()));
  if (editor_placeholder_image_) {
    engine->addImageProvider(QString::fromUtf8(kEditorPlaceholderImageProviderId),
                             new EditorPlaceholderImageProvider(editor_placeholder_image_));
  }

  if (library_ == nullptr) {
    return;
  }

  auto store = library_->thumbs().image_store();
  if (!store) {
    return;
  }

  // QQmlEngine takes ownership of the provider.
  engine->addImageProvider(QString::fromUtf8(kThumbnailImageProviderId),
                           new ThumbnailImageProvider(std::move(store)));
}

void ApplicationModuleHost::AttachDisplayWindow(QWindow* window) {
  if (editor_comparison_) {
    editor_comparison_->SetDisplayWindow(window);
  }
  if (lut_preview_) {
    lut_preview_->SetDisplayWindow(window);
  }
  if (preset_preview_) {
    preset_preview_->SetDisplayWindow(window);
  }
  if (preset_dialog_preview_) {
    preset_dialog_preview_->SetDisplayWindow(window);
  }
  if (library_) {
    library_->SetDisplayWindow(window);
  }
}

void ApplicationModuleHost::RecordConstruction(const char* type_name, const void* object) {
  if (lifecycle_observer_) {
    lifecycle_observer_(LifecycleEvent{LifecycleEvent::Kind::Constructed, type_name, object});
  }
}

void ApplicationModuleHost::RecordDestruction(const char* type_name, const void* object) {
  if (lifecycle_observer_) {
    lifecycle_observer_(LifecycleEvent{LifecycleEvent::Kind::Destroyed, type_name, object});
  }
}

}  // namespace alcedo::ui
