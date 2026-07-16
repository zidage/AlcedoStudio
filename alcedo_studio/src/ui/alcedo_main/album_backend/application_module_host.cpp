//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/application_module_host.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {

using namespace std::chrono_literals;

constexpr auto kImageAnalysisSidecarStartupTimeout = 60s;

namespace {

auto TrimAscii(std::string value) -> std::string {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

auto CleanContextValue(std::string value) -> std::string {
  value = TrimAscii(std::move(value));
  for (char& ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
  }
  return value;
}

auto FormatOneDecimal(float value) -> std::string {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << value;
  std::string out = ss.str();
  while (out.size() > 1 && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out;
}

auto FormatShutterSpeed(std::pair<int, int> shutter) -> std::string {
  const auto [num, den] = shutter;
  if (num <= 0 || den <= 0) {
    return {};
  }
  if (den == 1) {
    return std::to_string(num) + "s";
  }
  return std::to_string(num) + "/" + std::to_string(den) + "s";
}

void AppendContextField(std::vector<std::string>* fields, std::string key, std::string value) {
  if (fields == nullptr) {
    return;
  }
  value = CleanContextValue(std::move(value));
  if (!value.empty()) {
    fields->push_back(std::move(key) + "=" + std::move(value));
  }
}

auto BuildCameraContext(const ExifDisplayMetaData& exif) -> std::string {
  std::vector<std::string> fields;
  fields.reserve(12);
  AppendContextField(&fields, "camera_make", exif.make_);
  AppendContextField(&fields, "camera_model", exif.model_);
  AppendContextField(&fields, "lens_make", exif.lens_make_);
  AppendContextField(&fields, "lens_model", exif.lens_);
  if (std::isfinite(exif.aperture_) && exif.aperture_ > 0.0f) {
    AppendContextField(&fields, "aperture", "f/" + FormatOneDecimal(exif.aperture_));
  }
  AppendContextField(&fields, "shutter_speed", FormatShutterSpeed(exif.shutter_speed_));
  if (exif.iso_ > 0) {
    AppendContextField(&fields, "iso", std::to_string(exif.iso_));
  }
  if (std::isfinite(exif.focal_) && exif.focal_ > 0.0f) {
    AppendContextField(&fields, "focal_length", FormatOneDecimal(exif.focal_) + "mm");
  }
  if (std::isfinite(exif.focal_35mm_) && exif.focal_35mm_ > 0.0f) {
    AppendContextField(&fields, "focal_length_35mm", FormatOneDecimal(exif.focal_35mm_) + "mm");
  }
  if (std::isfinite(exif.focus_distance_m_) && exif.focus_distance_m_ > 0.0f) {
    AppendContextField(&fields, "focus_distance", FormatOneDecimal(exif.focus_distance_m_) + "m");
  }
  if (exif.width_ > 0 && exif.height_ > 0) {
    AppendContextField(&fields, "image_size",
                       std::to_string(exif.width_) + "x" + std::to_string(exif.height_));
  }
  AppendContextField(&fields, "captured_at", exif.date_time_str_);
  if (exif.is_hdr_) {
    AppendContextField(&fields, "hdr", "true");
  }
  if (fields.empty()) {
    return {};
  }
  std::ostringstream ss;
  ss << "Camera/EXIF metadata for this image: ";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      ss << "; ";
    }
    ss << fields[i];
  }
  ss << ".";
  return ss.str();
}

}  // namespace

// ── Production IImageAnalysisEnvironment (no host dependency) ───────────────
class AlbumImageAnalysisEnvironment final : public IImageAnalysisEnvironment {
 public:
  AlbumImageAnalysisEnvironment(ProjectModule* project, SemanticGenerationController* semantic,
                                alcedo::AiProviderProfileController* profiles,
                                std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate)
      : project_(project), semantic_(semantic), profiles_(profiles), gate_(std::move(gate)) {}

  auto ThumbnailProvider() -> std::shared_ptr<IImageAnalysisThumbnailProvider> override {
    if (thumbnail_provider_) {
      return thumbnail_provider_;
    }
    if (!project_) {
      return nullptr;
    }
    auto ts = project_->handler().thumbnail_service();
    if (!ts) {
      return nullptr;
    }
    thumbnail_provider_ = std::make_shared<ThumbnailServiceImageAnalysisProvider>(ts);
    return thumbnail_provider_;
  }

  auto AnalysisClient() -> std::shared_ptr<IImageAnalysisClient> override {
    if (!project_) {
      return nullptr;
    }
    auto project = project_->handler().project();
    if (!project) {
      return nullptr;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (!runtime) {
      return nullptr;
    }
    return std::make_shared<AiSidecarRuntimeImageAnalysisClient>(runtime);
  }

  auto CredentialStore() -> std::shared_ptr<IAiCredentialStore> override {
    return profiles_ ? profiles_->CredentialStore() : nullptr;
  }

  auto Gate() -> std::shared_ptr<ImageAnalysisInFlightGate> override { return gate_; }

  auto CameraContextForItem(const ImageAnalysisItem& item) -> std::string override {
    if (!project_ || item.image_id == 0) {
      return {};
    }
    auto project = project_->handler().project();
    if (!project) {
      return {};
    }
    auto image_pool = project->GetImagePoolService();
    if (!image_pool) {
      return {};
    }
    std::string context;
    image_pool->Read<void>(item.image_id, [&context](const std::shared_ptr<Image>& image) {
      if (!image || !image->has_exif_display_.load()) {
        return;
      }
      context = BuildCameraContext(image->exif_display_);
    });
    return context;
  }

  auto AcquireSidecarLease() -> std::shared_ptr<void> override {
    if (!project_) {
      return {};
    }
    auto project = project_->handler().project();
    if (!project) {
      return {};
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    return runtime ? runtime->AcquireLease() : std::shared_ptr<void>{};
  }

  auto EnsureSidecarReady(bool provider_configs_dirty, std::string* error) -> bool override {
    (void)provider_configs_dirty;
    if (!project_) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto project = project_->handler().project();
    if (!project) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (!runtime) {
      if (error) {
        *error = "ai sidecar runtime service is unavailable";
      }
      return false;
    }
    if (runtime->Status().state == AiSidecarRuntimeState::kReady) {
      return true;
    }
    AiSidecarRuntimeOptions options =
        semantic_ ? semantic_->RuntimeOptionsForCurrentSidecarSnapshot(false)
                  : AiSidecarRuntimeOptions{};
    options.allow_download     = false;
    options.require_model_info = false;
    options.startup_timeout    = kImageAnalysisSidecarStartupTimeout;
    if (!runtime->StartAndWait(options)) {
      if (error) {
        *error = runtime->Status().message;
        if (error->empty()) {
          *error = "ai sidecar failed to start";
        }
      }
      return false;
    }
    return runtime->Status().state == AiSidecarRuntimeState::kReady;
  }

  auto EnsureSidecarReadyInteractive(bool provider_configs_dirty, std::string* error)
      -> bool override {
    (void)provider_configs_dirty;
    if (!project_) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto project = project_->handler().project();
    if (!project) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (!runtime) {
      if (error) {
        *error = "ai sidecar runtime service is unavailable";
      }
      return false;
    }
    if (runtime->Status().state == AiSidecarRuntimeState::kReady) {
      return true;
    }
    AiSidecarRuntimeOptions options =
        semantic_ ? semantic_->RuntimeOptionsForCurrentSidecarSnapshot(false)
                  : AiSidecarRuntimeOptions{};
    options.allow_download     = false;
    options.require_model_info = false;
    options.startup_timeout    = kImageAnalysisSidecarStartupTimeout;
    if (!runtime->StartAndWaitInteractive(options)) {
      if (error) {
        *error = runtime->Status().message;
        if (error->empty()) {
          *error = "ai sidecar failed to start";
        }
      }
      return false;
    }
    return runtime->Status().state == AiSidecarRuntimeState::kReady;
  }

  void RequestSidecarStartCancel() override {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (runtime) {
      runtime->RequestCancelStart();
    }
  }

 private:
  ProjectModule*                                   project_  = nullptr;
  SemanticGenerationController*                    semantic_ = nullptr;
  alcedo::AiProviderProfileController*             profiles_ = nullptr;
  std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate_;
  std::shared_ptr<IImageAnalysisThumbnailProvider> thumbnail_provider_;
};

std::shared_ptr<IImageAnalysisEnvironment> MakeAlbumImageAnalysisEnvironment(
    ProjectModule* project, SemanticGenerationController* semantic,
    alcedo::AiProviderProfileController* profiles,
    std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate) {
  return std::make_shared<AlbumImageAnalysisEnvironment>(project, semantic, profiles,
                                                         std::move(gate));
}

// ── Production IImageAnalysisSink ───────────────────────────────────────────
class AlbumImageAnalysisSink final : public IImageAnalysisSink {
 public:
  AlbumImageAnalysisSink(ProjectModule* project, ImageController* images, StatsEngine* stats,
                         ProjectDbWriteBarrier* barrier)
      : project_(project), images_(images), stats_(stats), queue_(*barrier) {}

  static constexpr const char* kDescribeTaskId = "describe";
  static constexpr const char* kScoreTaskId    = "rate";

  bool PersistUnderstanding(const ImageAnalysisItemResult& result) override {
    queue_.Submit([this, result] { DoPersistUnderstanding(result); });
    return true;
  }

  size_t PersistUnderstandings(const std::vector<ImageAnalysisItemResult>& results) override {
    const size_t n = results.size();
    queue_.Submit([this, results] { DoPersistUnderstandings(results); });
    return n;
  }

  bool PersistRatingReasons(const ImageAnalysisItemResult& result) override {
    queue_.Submit([this, result] { DoPersistRatingReasons(result); });
    return true;
  }

  bool ApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) override {
    queue_.Submit(
        [this, elementId, imageId, rating] { DoApplyStarRating(elementId, imageId, rating); });
    return true;
  }

  void FlushPendingStarRatings() override {
    queue_.Submit([this] { DoFlushPendingStarRatings(); });
  }

  void NotifySearchDocumentChanged() override {
    queue_.Submit([this] { DoNotifySearchDocumentChanged(); });
  }

  bool HasPendingWrites() const override { return queue_.IsPending(); }
  void SetOnDrainComplete(std::function<void()> cb) override {
    queue_.SetOnDrainComplete(std::move(cb));
  }
  void FlushPendingWrites() override { queue_.Drain(); }

 private:
  void DoPersistUnderstanding(const ImageAnalysisItemResult& result) {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto& ai = project->GetStorageService()->GetAiStorageController();
    (void)ai.UpsertUnderstanding(MakeDescription(result));
  }

  void DoPersistUnderstandings(const std::vector<ImageAnalysisItemResult>& results) {
    if (!project_ || results.empty()) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    std::vector<AiDescription> descriptions;
    descriptions.reserve(results.size());
    for (const auto& result : results) {
      descriptions.push_back(MakeDescription(result));
    }
    auto& ai = project->GetStorageService()->GetAiStorageController();
    (void)ai.UpsertUnderstandings(descriptions);
  }

  void DoPersistRatingReasons(const ImageAnalysisItemResult& result) {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto&    ai = project->GetStorageService()->GetAiStorageController();
    AiRating r;
    r.file_id_           = result.item.element_id;
    r.task_id_           = kScoreTaskId;
    r.provider_id_       = result.rating.provider;
    r.model_id_          = result.rating.model_id;
    r.prompt_profile_id_ = result.rating.prompt_profile_id;
    r.rendition_kind_    = result.rating.rendition.kind;
    r.rating_            = 0;
    r.rubric_id_         = result.rating.rubric_id;
    r.rubric_version_    = result.rating.rubric_version;
    r.reasons_           = result.rating.reasons;
    r.active_            = true;
    (void)ai.UpsertRatingReasons(r);
  }

  void DoApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) {
    if (images_) {
      images_->ApplyStarRatingLight(elementId, imageId, rating);
    }
  }

  void DoFlushPendingStarRatings() {
    if (images_) {
      images_->FlushPendingStarRatings();
    }
  }

  void DoNotifySearchDocumentChanged() {
    if (stats_) {
      stats_->RebuildThumbnailView();
    }
  }

  static auto MakeDescription(const ImageAnalysisItemResult& result) -> AiDescription {
    AiDescription d;
    d.file_id_           = result.item.element_id;
    d.task_id_           = kDescribeTaskId;
    d.provider_id_       = result.understanding.provider;
    d.model_id_          = result.understanding.model_id;
    d.prompt_profile_id_ = result.understanding.prompt_profile_id;
    d.rendition_kind_    = result.understanding.rendition.kind;
    d.caption_           = result.understanding.caption;
    d.scene_             = result.understanding.scene;
    d.confidence_        = result.understanding.confidence;
    d.active_            = true;
    d.SetTags(result.understanding.tags);
    return d;
  }

  ProjectModule*           project_ = nullptr;
  ImageController*         images_  = nullptr;
  StatsEngine*             stats_   = nullptr;
  AnalysisResultWriteQueue queue_;
};

std::shared_ptr<IImageAnalysisSink> MakeAlbumImageAnalysisSink(ProjectModule* project,
                                                               ImageController* images,
                                                               StatsEngine* stats,
                                                               ProjectDbWriteBarrier* barrier) {
  return std::make_shared<AlbumImageAnalysisSink>(project, images, stats, barrier);
}

// ── ApplicationModuleHost ───────────────────────────────────────────────────

auto ApplicationModuleHost::ConstructionOrder() -> std::vector<std::string> {
  return {
      "BackgroundTaskController",
      "InteractionPolicyController",
      "ModelDownloadService",
      "ProjectModule",
      "LibraryModule",
      "FolderController",
      "ImageController",
      "StatsEngine",
      "SearchController",
      "ModelDownloadController",
      "AiProviderProfileController",
      "SemanticGenerationController",
      "ImageAnalysisInFlightGate",
      "ProjectDbWriteBarrier",
      "ImageAnalysisSink",
      "ImageAnalysisController",
      "ImportExportHandler",
      "NikonHeRecoveryController",
      "EditorController",
      "AdjustmentTransferController",
  };
}

auto ApplicationModuleHost::DestructionOrder() -> std::vector<std::string> {
  auto order = ConstructionOrder();
  std::reverse(order.begin(), order.end());
  return order;
}

ApplicationModuleHost::ApplicationModuleHost(QObject* parent) : QObject(parent) {
  // Construction dependency order (see ConstructionOrder()).
  background_tasks_    = std::make_unique<BackgroundTaskController>();
  interaction_policy_  = std::make_unique<InteractionPolicyController>(background_tasks_.get(), this);
  model_download_service_ = std::make_unique<alcedo::ModelDownloadService>();
  project_             = std::make_unique<ProjectModule>(this);
  library_             = std::make_unique<LibraryModule>(project_.get(), this);
  folders_ =
      std::make_unique<FolderController>(project_.get(), library_.get(), project_.get(), this);
  images_ = std::make_unique<ImageController>(project_.get(), library_.get(), folders_.get(),
                                              project_.get(), this);
  stats_  = std::make_unique<StatsEngine>(project_.get(), library_.get(), folders_.get(), this);
  search_ = std::make_unique<SearchController>(project_.get(), library_.get(), folders_.get(),
                                               stats_.get(), this);
  model_download_ = std::make_unique<ModelDownloadController>(*model_download_service_,
                                                              background_tasks_.get(), this);
  ai_provider_profiles_ = std::make_unique<alcedo::AiProviderProfileController>(this);
  semantic_generation_  = std::make_unique<SemanticGenerationController>(
      project_.get(), library_.get(), model_download_.get(), background_tasks_.get(),
      project_.get(), ai_provider_profiles_.get(), this);
  image_analysis_gate_ = std::make_shared<alcedo::ImageAnalysisInFlightGate>();
  db_write_barrier_    = std::make_unique<ProjectDbWriteBarrier>();
  image_analysis_sink_ =
      MakeAlbumImageAnalysisSink(project_.get(), images_.get(), stats_.get(), db_write_barrier_.get());
  image_analysis_ = std::make_unique<ImageAnalysisController>(
      MakeAlbumImageAnalysisEnvironment(project_.get(), semantic_generation_.get(),
                                        ai_provider_profiles_.get(), image_analysis_gate_),
      ai_provider_profiles_.get(), image_analysis_sink_, background_tasks_.get());
  import_export_ = std::make_unique<ImportExportHandler>(
      project_.get(), library_.get(), folders_.get(), project_.get(), db_write_barrier_.get(), this);
  nikon_he_recovery_ = std::make_unique<NikonHeRecoveryController>(
      project_.get(), images_.get(), project_.get(), this);
  editor_ = std::make_unique<EditorController>(project_.get(), library_.get(), this);
  adjustment_transfer_ = std::make_unique<AdjustmentTransferController>(
      project_.get(), library_.get(), import_export_.get(), this);

  // Late collaborator binding (breaks residual construction cycles).
  project_->BindCollaborators(import_export_.get(), editor_.get(), library_.get(), folders_.get(),
                              stats_.get(), semantic_generation_.get(), model_download_.get());
  library_->BindCollaborators(folders_.get(), search_.get(), stats_.get());
  folders_->BindCollaborators(stats_.get(), search_.get(), import_export_.get());
  images_->BindCollaborators(stats_.get(), import_export_.get(), editor_.get(),
                             semantic_generation_.get(), interaction_policy_.get());
  stats_->BindCollaborators(search_.get(), semantic_generation_.get());
  semantic_generation_->BindCollaborators(nikon_he_recovery_.get());
  import_export_->BindCollaborators(stats_.get(), nikon_he_recovery_.get(),
                                    semantic_generation_.get());
  nikon_he_recovery_->BindCollaborators(import_export_.get(), semantic_generation_.get());

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

  db_write_barrier_->SetOnRelease({});

  try {
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
    if (import_export_) {
      auto job = import_export_->current_import_job();
      if (job) {
        job->canceled_.store(true);
      }
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
  // unique_ptr members destroy in reverse declaration order automatically.
}

}  // namespace alcedo::ui
