//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/application_module_host.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "app/editor_session_bootstrap.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_session_production.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::ui {

// ── ApplicationModuleHost ───────────────────────────────────────────────────

ApplicationModuleHost::ApplicationModuleHost(QObject* parent, LifecycleObserver observer)
    : QObject(parent), lifecycle_observer_(std::move(observer)) {
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();
  alcedo::ui::RegisterEditorAdjustmentQmlTypes();
  background_tasks_ = std::make_unique<BackgroundTaskController>();
  RecordConstruction("BackgroundTaskController", background_tasks_.get());
  interaction_policy_ =
      std::make_unique<InteractionPolicyController>(background_tasks_.get(), this);
  RecordConstruction("InteractionPolicyController", interaction_policy_.get());
  model_download_service_ = std::make_unique<alcedo::ModelDownloadService>();
  RecordConstruction("ModelDownloadService", model_download_service_.get());
  project_ = std::make_unique<ProjectModule>(this);
  RecordConstruction("ProjectModule", project_.get());
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
  ai_provider_profiles_ = std::make_unique<alcedo::AiProviderProfileController>(this);
  RecordConstruction("AiProviderProfileController", ai_provider_profiles_.get());
  semantic_generation_ = std::make_unique<SemanticGenerationController>(
      project_.get(), library_.get(), model_download_.get(), background_tasks_.get(),
      project_.get(), ai_provider_profiles_.get(), this);
  RecordConstruction("SemanticGenerationController", semantic_generation_.get());
  image_analysis_gate_ = std::make_shared<alcedo::ImageAnalysisInFlightGate>();
  RecordConstruction("ImageAnalysisInFlightGate", image_analysis_gate_.get());
  db_write_barrier_ = std::make_unique<ProjectDbWriteBarrier>();
  RecordConstruction("ProjectDbWriteBarrier", db_write_barrier_.get());
  image_analysis_sink_ = MakeAlbumImageAnalysisSink(project_.get(), images_.get(), stats_.get(),
                                                    db_write_barrier_.get());
  RecordConstruction("ImageAnalysisSink", image_analysis_sink_.get());
  image_analysis_ = std::make_unique<ImageAnalysisController>(
      MakeAlbumImageAnalysisEnvironment(project_.get(), semantic_generation_.get(),
                                        ai_provider_profiles_.get(), image_analysis_gate_),
      ai_provider_profiles_.get(), image_analysis_sink_, background_tasks_.get());
  RecordConstruction("ImageAnalysisController", image_analysis_.get());
  import_export_ =
      std::make_unique<ImportExportHandler>(project_.get(), library_.get(), folders_.get(),
                                            project_.get(), db_write_barrier_.get(), this);
  RecordConstruction("ImportExportHandler", import_export_.get());
  nikon_he_recovery_ = std::make_unique<NikonHeRecoveryController>(project_.get(), images_.get(),
                                                                   project_.get(), this);
  RecordConstruction("NikonHeRecoveryController", nikon_he_recovery_.get());
  editor_ = std::make_unique<EditorController>(project_.get(), library_.get(), this);
  RecordConstruction("EditorController", editor_.get());
  adjustment_transfer_ = std::make_unique<AdjustmentTransferController>(
      project_.get(), library_.get(), import_export_.get(), this);
  RecordConstruction("AdjustmentTransferController", adjustment_transfer_.get());
  // Phase 5B/5E: wire production first-frame ports, direct presentation, and
  // BackgroundTaskController-backed editor_save registration into the session
  // runtime. The production scheduler accepts intents without completing when
  // no real image path / test producer is available (shell-compatible Loading).
  {
    auto production_pipeline  = std::make_shared<EditorSessionProductionPipelinePort>();
    auto production_history   = std::make_shared<EditorSessionProductionHistoryPort>();
    auto production_tasks =
        std::make_shared<EditorSessionProductionTaskPort>(background_tasks_.get());
    auto production_scheduler = std::make_shared<EditorSessionProductionSchedulerPort>();
    production_scheduler->SetPipelinePort(production_pipeline);

    EditorSessionProductionServices production_services;
    production_services.pipeline_service =
        [this]() -> std::shared_ptr<alcedo::PipelineMgmtService> {
      return project_ ? project_->handler().pipeline_service() : nullptr;
    };
    production_services.history_service =
        [this]() -> std::shared_ptr<alcedo::EditHistoryMgmtService> {
      return project_ ? project_->handler().history_service() : nullptr;
    };
    production_services.image_pool = [this]() -> std::shared_ptr<alcedo::ImagePoolService> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      return project_->handler().project()->GetImagePoolService();
    };
    production_services.storage_service = [this]() -> std::shared_ptr<alcedo::StorageService> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      return project_->handler().project()->GetStorageService();
    };
    production_services.load_history =
        [this](sl_element_id_t element_id) -> std::shared_ptr<alcedo::EditHistory> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      auto storage = project_->handler().project()->GetStorageService();
      if (!storage) {
        return nullptr;
      }
      try {
        auto history = storage->GetElementController().GetEditHistoryByFileId(element_id);
        // A fresh image has no durable history row yet; recovery layers on top
        // of a fresh EditHistory so a crash before the first materialize can still
        // REDO its journal-committed operations.
        return history ? history : std::make_shared<alcedo::EditHistory>(element_id);
      } catch (...) {
        return nullptr;
      }
    };
    production_services.load_pipeline =
        [this](sl_element_id_t element_id) -> std::shared_ptr<alcedo::CPUPipelineExecutor> {
      if (!project_ || !project_->handler().project()) {
        return nullptr;
      }
      auto storage = project_->handler().project()->GetStorageService();
      if (!storage) {
        return nullptr;
      }
      try {
        return storage->GetElementController().GetPipelineByElementId(element_id);
      } catch (...) {
        return nullptr;
      }
    };
    production_services.journal_path = [this](sl_element_id_t element_id) {
      if (!project_) {
        return std::filesystem::path{};
      }
      auto root = project_->handler().db_path().parent_path();
      if (root.empty()) {
        root = project_->handler().workspace_dir();
      }
      if (root.empty()) {
        return std::filesystem::path{};
      }
      return root / "editor-journal" /
             ("image-" + std::to_string(static_cast<std::uint64_t>(element_id)) + ".wal");
    };
    production_services.invalidate_thumbnail = [this](sl_element_id_t element_id) {
      if (!project_) {
        return;
      }
      if (auto thumbnails = project_->handler().thumbnail_service()) {
        thumbnails->InvalidateThumbnail(element_id);
      }
    };
    production_pipeline->SetServices(production_services);
    production_history->SetServices(production_services);
    production_scheduler->SetServices(production_services);
    auto production_journal =
        std::make_shared<EditorSessionProductionJournalPort>(production_services);

    editor_session_runtime_ = alcedo::EditorSessionRuntime::CreateWithPorts(
        production_pipeline, production_history, production_tasks, production_journal,
        production_scheduler);
    production_scheduler->SetCoordinator(editor_session_runtime_->coordinator);
    editor_session_production_scheduler_ = std::move(production_scheduler);
  }
  RecordConstruction("EditorSessionService", editor_session_runtime_->service.get());
  RecordConstruction("EditorRenderCoordinator", editor_session_runtime_->coordinator.get());
  editor_session_ = std::make_unique<EditorSessionController>(
      editor_.get(), editor_session_runtime_->service.get(), this);
  RecordConstruction("EditorSessionController", editor_session_.get());
  if (editor_session_production_scheduler_) {
    editor_session_production_scheduler_->SetSinkResolver([this]() -> alcedo::IFrameSink* {
      return editor_session_ ? editor_session_->presentation_frame_sink() : nullptr;
    });
  }
  workspace_router_ = std::make_unique<WorkspaceRouter>(editor_session_.get(), this);
  RecordConstruction("WorkspaceRouter", workspace_router_.get());

  editor_->InitializeEditorLuts();
  library_->BindCollaborators(folders_.get(), search_.get(), stats_.get());
  library_->SetSemanticLabelProvider(
      [semantic = semantic_generation_.get()](sl_element_id_t element_id) {
        return semantic ? semantic->LabelDisplayText(element_id) : QString{};
      });
  folders_->BindCollaborators(stats_.get(), search_.get(), import_export_.get());
  images_->BindCollaborators(stats_.get(), import_export_.get(), editor_.get(),
                             semantic_generation_.get(), interaction_policy_.get());
  stats_->BindCollaborators(search_.get(), semantic_generation_.get());
  semantic_generation_->BindCollaborators(nikon_he_recovery_.get());
  import_export_->BindCollaborators(stats_.get(), nikon_he_recovery_.get(),
                                    semantic_generation_.get());
  nikon_he_recovery_->BindCollaborators(import_export_.get(), semantic_generation_.get());

  ProjectLifecycleHooks lifecycle_hooks;
  lifecycle_hooks.project_switch_block_reason = [import_export = import_export_.get()] {
    if (import_export && import_export->current_import_job() &&
        !import_export->current_import_job()->IsCancelationAcked()) {
      return QStringLiteral("Cannot switch project while an import is running.");
    }
    if (import_export && import_export->export_inflight()) {
      return QStringLiteral("Cannot switch project while export is running.");
    }
    return QString{};
  };
  lifecycle_hooks.finalize_editor_session = [editor           = editor_.get(),
                                             editor_session   = editor_session_.get(),
                                             workspace_router = workspace_router_.get()] {
    // Project switch/close must end both the legacy controller session (if any)
    // and the unified QML workspace session, then return to the library route
    // so stale element/image ids never survive into the next project.
    if (editor && editor->editor_active()) {
      editor->FinalizeEditorSession(true);
    }
    // Forget the last-edited image so re-entering the editor in the next
    // project does not restore an image that belongs to the old project.
    if (editor_session) {
      editor_session->clearLastEditedImage();
    }
    if (workspace_router) {
      // OpenLibrary finalizes an active EditorSessionController session.
      workspace_router->OpenLibrary();
    } else if (editor_session && editor_session->active()) {
      editor_session->Finalize(true);
    }
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
                                    semantic = semantic_generation_.get()] {
    const auto preferred_folder_path =
        folders ? folders->current_folder_path() : std::filesystem::path{};
    if (import_export) {
      import_export->ResetExportState();
    }
    if (library) {
      library->ReloadFolderTree(preferred_folder_path);
    }
    if (stats) {
      stats->ClearFilters();
    }
    if (library) {
      library->ReloadCurrentFolder();
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
  lifecycle_hooks.refresh_translations =
      [folders = folders_.get(), library = library_.get(), stats = stats_.get(),
       import_export = import_export_.get(), editor = editor_.get()] {
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
        if (editor) {
          emit editor->EditorStateChanged();
        }
      };
  project_->SetLifecycleHooks(std::move(lifecycle_hooks));

  db_write_barrier_->SetOnRelease([this] {
    if (image_analysis_sink_) {
      image_analysis_sink_->FlushPendingWrites();
    }
  });
}

void ApplicationModuleHost::ShutdownModules() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  try {
    if (background_tasks_) {
      background_tasks_->CancelAll();
    }
    if (semantic_generation_) {
      semantic_generation_->CancelGeneration();
    }
    if (search_) {
      search_->CancelSearchPreviewThumbnails();
    }
    if (library_) {
      library_->thumbs().ReleaseVisibleThumbnailPins();
    }
    if (editor_) {
      editor_->FinalizeEditorSession(true);
    }
    if (workspace_router_) {
      workspace_router_->OpenLibrary();
    } else if (editor_session_ && editor_session_->active()) {
      editor_session_->Finalize(true);
    }
    if (editor_session_) {
      editor_session_->Shutdown();
    }
    if (import_export_) {
      import_export_->CancelImport();
    }

    // Deliver cancellation/finalization callbacks while every module is still
    // alive. Export is a wait-for-finish task; its completion releases the DB
    // barrier and the installed callback drains analysis results immediately.
    const QDeadlineTimer deadline(15000);
    while (((background_tasks_ && background_tasks_->RunningCount() > 0) ||
            (import_export_ &&
             (import_export_->ImportRunning() || import_export_->export_inflight()))) &&
           !deadline.hasExpired()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    if (image_analysis_sink_ && (!db_write_barrier_ || !db_write_barrier_->IsHeld())) {
      image_analysis_sink_->FlushPendingWrites();
    }
    if (project_) {
      auto psvc = project_->handler().pipeline_service();
      if (psvc) {
        psvc->Sync();
      }
      (void)project_->handler().PurgeUninstalledSemanticModels();
      if (project_->handler().PersistCurrentProjectState()) {
        QString ignored_error;
        (void)project_->handler().PackageCurrentProjectFiles(&ignored_error);
      }
      album_util::CleanupWorkspaceDirectory(project_->handler().workspace_dir());
    }
  } catch (...) {
  }
}

ApplicationModuleHost::~ApplicationModuleHost() {
  ShutdownModules();

  // Destroy explicitly so the lifecycle observer records the actual object
  // lifetime, including non-QObject infrastructure, and so dependent modules
  // are visibly torn down before the services they reference.
  auto destroy = [this](auto& pointer, const char* type_name) {
    const void* object = pointer.get();
    pointer.reset();
    RecordDestruction(type_name, object);
  };
  auto destroy_shared = [this](auto& pointer, const char* type_name) {
    const void* object = pointer.get();
    pointer.reset();
    RecordDestruction(type_name, object);
  };
  destroy(workspace_router_, "WorkspaceRouter");
  destroy(editor_session_, "EditorSessionController");
  if (editor_session_runtime_) {
    RecordDestruction("EditorRenderCoordinator", editor_session_runtime_->coordinator.get());
    RecordDestruction("EditorSessionService", editor_session_runtime_->service.get());
    editor_session_runtime_.reset();
  }
  destroy(adjustment_transfer_, "AdjustmentTransferController");
  destroy(editor_, "EditorController");
  destroy(nikon_he_recovery_, "NikonHeRecoveryController");
  destroy(import_export_, "ImportExportHandler");
  destroy(image_analysis_, "ImageAnalysisController");
  destroy_shared(image_analysis_sink_, "ImageAnalysisSink");
  destroy(db_write_barrier_, "ProjectDbWriteBarrier");
  destroy_shared(image_analysis_gate_, "ImageAnalysisInFlightGate");
  destroy(semantic_generation_, "SemanticGenerationController");
  destroy(ai_provider_profiles_, "AiProviderProfileController");
  destroy(model_download_, "ModelDownloadController");
  destroy(search_, "SearchController");
  destroy(stats_, "StatsEngine");
  destroy(images_, "ImageController");
  destroy(folders_, "FolderController");
  destroy(library_, "LibraryModule");
  destroy(project_, "ProjectModule");
  destroy(model_download_service_, "ModelDownloadService");
  destroy(interaction_policy_, "InteractionPolicyController");
  destroy(background_tasks_, "BackgroundTaskController");
}

void ApplicationModuleHost::Shutdown() { ShutdownModules(); }

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
