//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "app/album_browse_service.hpp"
#include "app/model_asset_catalog.hpp"
#include "storage/controller/semantic/semantic_label_config.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/nikon_he_recovery_controller.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"

namespace alcedo::ui {

using namespace std::chrono_literals;

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

constexpr auto   kSemanticGenerationImportPreferenceKey = "semantic/importGenerationPreference";
constexpr auto   kSemanticPreferenceAsk                 = "ask";
constexpr auto   kSemanticPreferenceAlways              = "always";
constexpr auto   kSemanticPreferenceNever               = "never";
constexpr auto   kAiSidecarRuntimeStartupTimeout        = 60s;
constexpr auto   kJinaClipProfileId                     = "jina-clip-v2-int8-multilingual";
constexpr auto   kSiglip2ProfileId                      = "siglip2-b32-256-multilingual";
constexpr auto   kSiglip2CoreMlProfileId                = "siglip2-base-256-coreml-macos";
constexpr size_t kMobileClipBatchSize                   = 64;
constexpr size_t kJinaClipBatchSize                     = 4;
constexpr size_t kSiglip2BatchSize                      = 8;
constexpr size_t kCoreMlImageBatchSize                  = 1;
constexpr size_t kCoreMlTextBatchSize                   = 8;

auto             SemanticModelKeyFromInfo(const AiSidecarRuntimeModelInfo& info) -> std::string {
  if (info.revision.empty()) {
    return info.model_id;
  }
  return info.model_id + "@" + info.revision;
}

auto NormalizedSemanticPreference(QString preference) -> QString {
  preference = preference.trimmed().toLower();
  if (preference == QLatin1String(kSemanticPreferenceAlways) ||
      preference == QLatin1String(kSemanticPreferenceNever)) {
    return preference;
  }
  return QString::fromLatin1(kSemanticPreferenceAsk);
}

auto ClampToInt(size_t value) -> int {
  return static_cast<int>(
      std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int>::max())));
}

auto QStringToPath(const QString& value) -> std::filesystem::path {
#ifdef _WIN32
  return std::filesystem::path(value.toStdWString());
#else
  return std::filesystem::path(value.toStdString());
#endif
}

auto FindProfileByModel(const std::string& profile_id, const std::string& model_id)
    -> const ModelProfileSpec* {
  for (const auto& profile : SemanticModelProfiles()) {
    if (profile_id == profile.profile_id || model_id == profile.model_id) {
      return &profile;
    }
  }
  return nullptr;
}

auto CurrentUiSemanticLabelLanguage() -> SemanticLabelLanguage {
  QString code =
      QSettings{}.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
  if (code.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
    code = QLocale::system().bcp47Name();
  }
  return code.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)
             ? SemanticLabelLanguage::kChinese
             : SemanticLabelLanguage::kEnglish;
}

auto ModelLabelLanguage(const AiSidecarRuntimeModelInfo& info) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(info.profile_id.empty() ? info.model_id : info.profile_id,
                                       info.language);
}

auto ModelLabelLanguage(const SemanticResolvedModelManifest& manifest) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(
      manifest.profile_id.empty() ? manifest.model_id : manifest.profile_id, manifest.language);
}

auto EmbeddingBatchSizeForProfile(const AiSidecarRuntimeModelInfo& info) -> size_t {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return kJinaClipBatchSize;
  }
  if (profile_id == kSiglip2CoreMlProfileId) {
    return kCoreMlImageBatchSize;
  }
  if (profile_id == kSiglip2ProfileId) {
    return kSiglip2BatchSize;
  }
  return kMobileClipBatchSize;
}

auto LabelPrototypeBatchSizeForProfile(const AiSidecarRuntimeModelInfo& info) -> size_t {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kSiglip2CoreMlProfileId) {
    return kCoreMlTextBatchSize;
  }
  return EmbeddingBatchSizeForProfile(info);
}

auto LabelPrototypeBatchSizeForProfile(const SemanticResolvedModelManifest& manifest) -> size_t {
  const AiSidecarRuntimeModelInfo info{.profile_id = manifest.profile_id,
                                       .model_id   = manifest.model_id};
  return LabelPrototypeBatchSizeForProfile(info);
}

auto EmbeddingTimeoutForProfile(const AiSidecarRuntimeModelInfo& info)
    -> std::chrono::milliseconds {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return 120s;
  }
  if (profile_id == kSiglip2CoreMlProfileId) {
    return 120s;
  }
  if (profile_id == kSiglip2ProfileId) {
    return 60s;
  }
  return 30s;
}

auto EmbeddingTimeoutForProfile(const SemanticResolvedModelManifest& manifest)
    -> std::chrono::milliseconds {
  const AiSidecarRuntimeModelInfo info{.profile_id = manifest.profile_id,
                                       .model_id   = manifest.model_id};
  return EmbeddingTimeoutForProfile(info);
}

auto ItemsNeedingSemanticGeneration(const std::vector<SemanticGenerationItem>& items,
                                    SemanticStorageController&                 semantic,
                                    const std::string& model_key, bool force_regenerate)
    -> std::vector<SemanticGenerationItem> {
  if (force_regenerate || model_key.empty()) {
    return items;
  }

  std::vector<SemanticGenerationItem> pending;
  pending.reserve(items.size());
  constexpr bool require_label = true;
  for (const auto& item : items) {
    if (!semantic.HasReadyImageEmbedding(item.element_id, item.image_id, model_key,
                                         require_label)) {
      pending.push_back(item);
    }
  }
  return pending;
}

}  // namespace

SemanticGenerationController::SemanticGenerationController(
    ProjectModule* project, LibraryModule* library, ModelDownloadController* model_download,
    BackgroundTaskController* background_tasks, IUiStatusSink* status,
    alcedo::AiProviderProfileController* ai_profiles, QObject* parent)
    : QObject(parent), project_(project), library_(library), model_download_(model_download),
      background_tasks_(background_tasks), status_(status), ai_profiles_(ai_profiles) {

  // The selected-model "active" badge depends on the download controller's
  // install state + selected profile, so recompute it whenever that changes
  // (selection change, download finish, delete, refresh) — not on every
  // progress tick, which is why we listen to SelectedModelInstallChanged.
  connect(model_download_,
          &ModelDownloadController::SelectedModelInstallChanged, this, [this]() {
            RecomputeSelectedModelActive();
            emit StateChanged();
          });
  // When the user picks a different model in the combo box, refresh the badge and
  // auto-activate it when its label prototypes are already cached, so the user does
  // not have to press Activate again for a model that was previously activated.
  connect(model_download_,
          &ModelDownloadController::SelectedModelProfileChanged, this,
          [this]() { TryAutoActivateSelectedModel(); });
}

void SemanticGenerationController::BindCollaborators(NikonHeRecoveryController* nikon) {
  nikon_ = nikon;
}

bool SemanticGenerationController::PromptVisible() const {
  return prompt_pending_ && !activate_prompt_pending_ && !running_ &&
         !(nikon_ && nikon_->is_active());
}

bool SemanticGenerationController::ActivatePromptVisible() const {
  return activate_prompt_pending_ && !running_;
}

void SemanticGenerationController::StartPendingGeneration(bool forceRegenerate) {
  StartGenerationForItems(pending_items_, forceRegenerate);
}

void SemanticGenerationController::SkipPendingGeneration(bool rememberChoice) {
  if (rememberChoice) {
    SetImportPreference(QString::fromLatin1(kSemanticPreferenceNever));
  }
  ClearPrompt();
  status_text_ = PL_TEXT("Semantic generation skipped.");
  emit StateChanged();
}

void SemanticGenerationController::DismissActivatePrompt() {
  activate_prompt_pending_ = false;
  // The queued import batch is dropped: if the user later generates from the
  // Settings panel, its "Generate" action covers all unlabeled images in the
  // album, including the just-imported ones.
  ClearPrompt();
  emit StateChanged();
}

QString SemanticGenerationController::ImportPreference() const {
  return NormalizedSemanticPreference(
      QSettings{}
          .value(QLatin1String(kSemanticGenerationImportPreferenceKey),
                 QLatin1String(kSemanticPreferenceAsk))
          .toString());
}

void SemanticGenerationController::SetImportPreference(const QString& preference) {
  QSettings{}.setValue(QLatin1String(kSemanticGenerationImportPreferenceKey),
                       NormalizedSemanticPreference(preference));
  emit StateChanged();
}

QString SemanticGenerationController::ActiveModelProfileId() const {
  auto project = project_->handler().project();
  if (!project) {
    return {};
  }
  std::string error;
  const auto  model =
      project->GetStorageService()->GetSemanticStorageController().ActiveModel(&error);
  if (!model.has_value()) {
    return {};
  }
  return QString::fromStdString(model->profile_id_.empty() ? model->model_id_ : model->profile_id_);
}

QString SemanticGenerationController::ActiveModelDisplayName() const {
  auto project = project_->handler().project();
  if (!project) {
    return PL_TEXT("No active model").Render();
  }
  std::string error;
  const auto  model =
      project->GetStorageService()->GetSemanticStorageController().ActiveModel(&error);
  if (!model.has_value()) {
    return PL_TEXT("No active model").Render();
  }
  if (const auto* profile = FindProfileByModel(model->profile_id_, model->model_id_)) {
    return QString::fromLatin1(profile->display_name);
  }
  return QString::fromStdString(model->model_id_);
}

QString SemanticGenerationController::ActiveModelKeyQString() const {
  return QString::fromStdString(ActiveModelKey());
}

auto SemanticGenerationController::RuntimeOptionsForCurrentSidecarSnapshot(
    bool requireModelInfo) const -> AiSidecarRuntimeOptions {
  QString profile_id = ActiveModelProfileId();
  if (profile_id.isEmpty()) {
    profile_id = model_download_->SelectedModelProfileId();
  }
  auto options               = RuntimeOptionsForProfile(profile_id, true);
  options.require_model_info = requireModelInfo;
  return options;
}

void SemanticGenerationController::RecomputeSelectedModelActive() {
  const QString active_profile = ActiveModelProfileId();
  selected_model_active_ =
      model_download_->SelectedModelInstalled() && !active_profile.isEmpty() &&
      active_profile == model_download_->SelectedModelProfileId();
}

void SemanticGenerationController::RefreshSemanticState() {
  model_download_->RefreshInstallState();
  RecomputeSelectedModelActive();
  RefreshAlbumSummary();
  // On project open / after a purge, also activate the persisted selected model if
  // it is already warm, so the model the user last picked is the one in use.
  TryAutoActivateSelectedModel();
  emit StateChanged();
}

void SemanticGenerationController::TryAutoActivateSelectedModel() {
  // Always recompute first so the badge is honest (this is the fix for the stale
  // "Active" that appeared after selecting a different installed model).
  RecomputeSelectedModelActive();
  emit StateChanged();

  if (selected_model_active_) {
    return;  // already the active model
  }
  if (running_ || model_activation_running_) {
    return;  // don't fight a running generation or activation
  }
  if (!model_download_->SelectedModelInstalled()) {
    return;
  }
  auto project = project_->handler().project();
  if (!project) {
    return;
  }

  // Only auto-switch when the selected model is already "warm" - its label
  // prototypes are cached. A cold model must be activated manually (Activate
  // generates the cache), because there is no data to route to yet.
  QString    manifest_error;
  const auto manifest =
      model_download_->LoadSelectedResolvedManifest(&manifest_error);
  if (!manifest.has_value()) {
    return;
  }
  const std::string model_key      = manifest->revision.empty()
                                         ? manifest->model_id
                                         : manifest->model_id + "@" + manifest->revision;
  const auto        label_language = ModelLabelLanguage(*manifest);
  const auto        prompt_hash    = SemanticPromptConfigHashForLanguage(label_language);

  auto&             semantic       = project->GetStorageService()->GetSemanticStorageController();
  const auto        query_count    = semantic.CountLabelQueries(prompt_hash);
  if (query_count == 0) {
    return;
  }
  const auto prototype_count = semantic.CountLabelPrototypes(model_key, prompt_hash);
  if (prototype_count < query_count) {
    return;  // cold -> user must press Activate to generate the label cache
  }

  // SetActiveModelKey is HasModel-guarded: it returns false before touching the
  // active flag if the model row isn't registered, so a missing row can't leave the
  // project with no active model. Warm implies a prior Activate registered the row.
  std::string error;
  if (!semantic.SetActiveModelKey(model_key, &error)) {
    return;
  }
  model_key_ = model_key;
  RecomputeSelectedModelActive();
  RefreshAlbumSummary();
  library_->ReloadCurrentFolder();  // routing changed -> refresh album view + labels
  model_download_->SetStatusText(
      PL_TEXT("%1 is active for this project. Label prompts are ready.", ActiveModelDisplayName()));
  emit StateChanged();
}

void SemanticGenerationController::ActivateSelectedModel() {
  if (running_) {
    model_download_->SetStatusText(
        PL_TEXT("Finish or cancel semantic generation before activating another model."));
    emit StateChanged();
    return;
  }
  if (model_activation_running_) {
    return;
  }
  const QString profile_id = model_download_->SelectedModelProfileId();

  QString       manifest_error;
  const auto    manifest =
      model_download_->LoadSelectedResolvedManifest(&manifest_error);
  if (!manifest.has_value()) {
    model_download_->SetStatusText(
        manifest_error.isEmpty() ? PL_TEXT("Install the selected model before activating it.")
                                 : PL_TEXT("Cannot activate model: %1", manifest_error));
    emit StateChanged();
    return;
  }

  auto project = project_->handler().project();
  if (!project) {
    model_download_->SetStatusText(
        PL_TEXT("Open a project before activating a semantic model."));
    emit StateChanged();
    return;
  }
  auto runtime = project->GetAiSidecarRuntimeService();
  if (!runtime) {
    model_download_->SetStatusText(
        PL_TEXT("Semantic runtime service is unavailable."));
    emit StateChanged();
    return;
  }

  const std::string model_key      = manifest->revision.empty()
                                         ? manifest->model_id
                                         : manifest->model_id + "@" + manifest->revision;
  const auto        label_language = ModelLabelLanguage(*manifest);

  model_activation_running_        = true;
  model_download_->SetStatusText(
      PL_TEXT("Activating model and preparing labels..."));
  emit StateChanged();
  // Phase 2: surface activation in the task bar and publish its interaction
  // locks (blocks model swap, a second generation, and model-file changes).
  model_activation_task_id_ = RegisterActivationTask();

  auto runtime_options      = RuntimeOptionsForProfile(profile_id, true);
  auto sidecar_lease        = runtime->AcquireLease();
  if (!sidecar_lease) {
    model_activation_running_ = false;
    model_download_->SetStatusText(
        PL_TEXT("Semantic runtime service is unavailable."));
    if (!model_activation_task_id_.isEmpty()) {
      background_tasks_->FinishTask(
          model_activation_task_id_, BackgroundTaskState::Failed,
          PL_TEXT("Semantic runtime service is unavailable.").Render());
      model_activation_task_id_.clear();
    }
    emit StateChanged();
    return;
  }
  QPointer<SemanticGenerationController> self(this);
  std::thread([self, project = std::move(project), runtime = std::move(runtime),
               manifest      = *manifest, model_key, label_language, runtime_options,
               sidecar_lease = std::move(sidecar_lease)]() mutable {
    bool        ok             = false;
    bool        prototype_warm = false;
    std::string message;

    auto        finish = [&]() {
      if (!self) {
        return;
      }
      QMetaObject::invokeMethod(
          self,
          [self, ok, prototype_warm, model_key, message = std::move(message)]() mutable {
            if (!self) {
              return;
            }
            self->model_activation_running_ = false;
            if (!self->model_activation_task_id_.isEmpty()) {
              self->background_tasks_->FinishTask(
                  self->model_activation_task_id_,
                  ok ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed,
                  self->model_download_->ModelDownloadStatusText());
              self->model_activation_task_id_.clear();
            }
            if (ok) {
              self->model_key_ = model_key;
              self->model_download_->SetStatusText(
                  prototype_warm
                      ? PL_TEXT("%1 is active for this project. Label prompts are ready.",
                                       self->ActiveModelDisplayName())
                      : PL_TEXT("%1 is active for this project.", self->ActiveModelDisplayName()));
              self->RecomputeSelectedModelActive();
              self->RefreshAlbumSummary();
              self->library_->ReloadCurrentFolder();
            } else {
              const QString detail = QString::fromUtf8(message.c_str());
              self->model_download_->SetStatusText(
                  detail.isEmpty() ? PL_TEXT("Semantic model activation failed.")
                                   : PL_TEXT("Semantic model activation failed: %1", detail));
            }
            emit self->StateChanged();
          },
          Qt::QueuedConnection);
    };

    auto&       semantic       = project->GetStorageService()->GetSemanticStorageController();
    const auto  prompt_hash    = SemanticPromptConfigHashForLanguage(label_language);
    const bool  already_active = semantic.ActiveModelKey() == model_key;
    std::string error;
    if (!semantic.UpsertModel(
            SemanticModelRecord{.model_key_     = model_key,
                                .model_id_      = manifest.model_id,
                                .revision_      = manifest.revision,
                                .embedding_dim_ = static_cast<int>(manifest.embedding_dimension),
                                .image_size_    = static_cast<int>(manifest.image_size),
                                .engine_id_     = manifest.engine_profile_id,
                                .profile_id_    = manifest.profile_id,
                                .supported_text_languages_json_ =
                                    SemanticSupportedTextLanguagesJson(label_language),
                                .prompt_config_hash_  = prompt_hash,
                                .asset_manifest_json_ = {},
                                .active_              = already_active},
            &error)) {
      message = error;
      finish();
      return;
    }

    const auto query_count     = semantic.CountLabelQueries(prompt_hash);
    const auto prototype_count = semantic.CountLabelPrototypes(model_key, prompt_hash);
    prototype_warm             = query_count > 0 && prototype_count >= query_count;
    if (query_count == 0 || prototype_count < query_count) {
      auto runtime_status = runtime->Status();
      if (runtime_status.state != AiSidecarRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        // Interactive boot: this runs on the worker thread, but
        // StartAndWaitInteractive marshals to the runtime's (UI) thread via
        // BlockingQueuedConnection and pumps the event loop there during the
        // readiness poll — so the UI stays responsive while the cold sidecar
        // comes up instead of freezing for the startup timeout. The worker
        // thread blocks here until the boot finishes; the runtime shared_ptr
        // captured by this lambda is the keepalive that prevents the service
        // from being destroyed mid-boot.
        if (!runtime->StartAndWaitInteractive(runtime_options)) {
          runtime_status = runtime->Status();
          message        = runtime_status.message.empty() ? "semantic runtime failed to start"
                                                          : runtime_status.message;
          finish();
          return;
        }
        runtime_status = runtime->Status();
      }
      if (runtime_status.state != AiSidecarRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        message = runtime_status.message.empty()
                      ? "semantic runtime did not report model information"
                      : runtime_status.message;
        finish();
        return;
      }
      auto embedder = std::make_shared<AiSidecarRuntimeImageEmbeddingClient>(runtime);
      SemanticGenerationPersistenceOptions persistence;
      persistence.storage_controller         = &semantic;
      persistence.model_key                  = model_key;
      persistence.prompt_config_hash         = prompt_hash;
      persistence.label_prototype_batch_size = LabelPrototypeBatchSizeForProfile(manifest);
      if (!SemanticGenerationService::EnsureLabelPrototypes(
              persistence, embedder, EmbeddingTimeoutForProfile(manifest), &error)) {
        message = error;
        finish();
        return;
      }
      prototype_warm = true;
    }

    if (!semantic.SetActiveModelKey(model_key, &error)) {
      message = error;
      finish();
      return;
    }

    ok = true;
    finish();
  }).detach();
}

void SemanticGenerationController::CancelGeneration() {
  if (job_) {
    job_->Cancel();
    if (!background_task_id_.isEmpty()) {
      background_tasks_->UpdateTaskState(background_task_id_,
                                                BackgroundTaskState::Canceling);
    }
    status_text_ = PL_TEXT("Cancelling semantic generation...");
    emit StateChanged();
    return;
  }
  if (!running_) {
    return;
  }
  // Boot phase: the interactive sidecar boot is still running (job_ is null).
  // Flag the cancel so the boot-failure path finishes the task as Canceled, and
  // ask the runtime to abort at its next poll checkpoint.
  start_canceled_ = true;
  auto project = project_->handler().project();
  auto runtime = project ? project->GetAiSidecarRuntimeService() : nullptr;
  if (runtime) {
    runtime->RequestCancelStart();
  }
  if (!background_task_id_.isEmpty()) {
    background_tasks_->UpdateTaskState(background_task_id_,
                                              BackgroundTaskState::Canceling);
  }
  status_text_ = PL_TEXT("Cancelling semantic generation...");
  emit StateChanged();
}

void SemanticGenerationController::RefreshAlbumSummary() {
  const auto previous_total     = album_total_count_;
  const auto previous_labeled   = album_labeled_count_;
  const auto previous_unlabeled = album_unlabeled_count_;
  const auto previous_summary   = album_summary_text_;

  auto       project            = project_->handler().project();
  auto       browse             = project ? project->GetAlbumBrowseService() : nullptr;
  if (!project || !browse) {
    album_total_count_     = 0;
    album_labeled_count_   = 0;
    album_unlabeled_count_ = 0;
    album_summary_text_    = PL_TEXT("Open a project before running AI content recognition.");
    if (previous_total != album_total_count_ || previous_labeled != album_labeled_count_ ||
        previous_unlabeled != album_unlabeled_count_ ||
        previous_summary.source_ != album_summary_text_.source_ ||
        previous_summary.args_ != album_summary_text_.args_) {
      emit StateChanged();
    }
    return;
  }

  album_total_count_   = static_cast<int>(std::min<size_t>(
      browse->CountFilesInFolderById(0), static_cast<size_t>(std::numeric_limits<int>::max())));

  const auto model_key = ActiveModelKey();
  if (model_key.empty()) {
    album_labeled_count_ = 0;
  } else {
    album_labeled_count_ = static_cast<int>(std::min<size_t>(
        project->GetStorageService()->GetSemanticStorageController().CountImageLabelsInFolder(
            0, model_key),
        static_cast<size_t>(std::numeric_limits<int>::max())));
  }
  album_unlabeled_count_ = std::max(0, album_total_count_ - album_labeled_count_);
  album_summary_text_    = PL_TEXT("%1 image(s) total. %2 already have labels, %3 need labels.",
                                   album_total_count_, album_labeled_count_, album_unlabeled_count_);
  if (previous_total != album_total_count_ || previous_labeled != album_labeled_count_ ||
      previous_unlabeled != album_unlabeled_count_ ||
      previous_summary.source_ != album_summary_text_.source_ ||
      previous_summary.args_ != album_summary_text_.args_) {
    emit StateChanged();
  }
}

void SemanticGenerationController::StartAlbumGeneration(bool forceRegenerate) {
  auto project = project_->handler().project();
  auto browse  = project ? project->GetAlbumBrowseService() : nullptr;
  if (!project || !browse) {
    status_text_ = PL_TEXT("Semantic generation is unavailable without an open project.");
    emit StateChanged();
    return;
  }

  std::vector<SemanticGenerationItem> items;
  const auto                          files = browse->ListFilesInFolderById(0);
  items.reserve(files.size());
  for (const auto& file : files) {
    if (file.file_id_ == 0 || file.image_id_ == 0) {
      continue;
    }
    items.push_back(SemanticGenerationItem{file.file_id_, file.image_id_});
  }

  StartGenerationForItems(std::move(items), forceRegenerate);
}

void SemanticGenerationController::QueuePrompt(std::vector<SemanticGenerationItem> items) {
  pending_items_  = std::move(items);
  prompt_pending_ = !pending_items_.empty();
  total_          = static_cast<int>(pending_items_.size());
  embedded_       = 0;
  skipped_        = 0;
  failed_         = 0;
  canceled_       = 0;
  if (prompt_pending_) {
    status_text_ = PL_TEXT("Generate semantic labels for %1 imported image(s)?", total_);
  }
  emit StateChanged();
  ResumeQueuedWorkflow();
}

void SemanticGenerationController::ResumeQueuedWorkflow() {
  if (!prompt_pending_ || running_ || (nikon_ && nikon_->is_active())) {
    emit StateChanged();
    return;
  }

  const QString preference =
      NormalizedSemanticPreference(QSettings{}
                                       .value(QLatin1String(kSemanticGenerationImportPreferenceKey),
                                              QLatin1String(kSemanticPreferenceAsk))
                                       .toString());
  // "never" suppresses both the generate prompt and the activate-model prompt —
  // users who opt out of AI features never see either.
  if (preference == QLatin1String(kSemanticPreferenceNever)) {
    activate_prompt_pending_ = false;
    ClearPrompt();
    // QueuePrompt already emitted StateChanged while prompt_pending_ was true,
    // which synchronously opened the dialog. Re-emit so the QML binding
    // re-evaluates promptVisible to false and the dialog actually closes;
    // otherwise the suppression is invisible to the UI.
    emit StateChanged();
    return;
  }
  // A fresh project (no model registered) can't generate labels yet. Route to
  // the activate-model dialog instead, for both "ask" and "always" — "always"
  // can only take effect once a model exists.
  if (IsFreshProject()) {
    activate_prompt_pending_ = true;
    emit StateChanged();
    return;
  }
  activate_prompt_pending_ = false;
  if (preference == QLatin1String(kSemanticPreferenceAlways)) {
    StartPendingGeneration(false);
    return;
  }
  emit StateChanged();
}

auto SemanticGenerationController::StoredModelKey() const -> std::string {
  auto project = project_->handler().project();
  if (!project) {
    return {};
  }
  return project->GetStorageService()->GetSemanticStorageController().ActiveModelKey();
}

bool SemanticGenerationController::IsFreshProject() const {
  auto project = project_->handler().project();
  if (!project) {
    return false;
  }
  return project->GetStorageService()->GetSemanticStorageController().ListModels().empty();
}

auto SemanticGenerationController::ActiveModelKey() const -> std::string {
  const auto stored = StoredModelKey();
  if (!stored.empty()) {
    return stored;
  }
  return model_key_;
}

auto SemanticGenerationController::LabelDisplayText(sl_element_id_t elementId) const -> QString {
  const auto model_key = ActiveModelKey();
  if (model_key.empty()) {
    return {};
  }
  auto project = project_->handler().project();
  if (!project) {
    return {};
  }
  std::string error;
  const auto  label =
      project->GetStorageService()->GetSemanticStorageController().GetImageLabelForFile(
          elementId, model_key, &error);
  if (!label.has_value()) {
    return {};
  }
  const auto display_language = CurrentUiSemanticLabelLanguage();
  const auto display_label    = [&](const std::string& value) {
    return QString::fromUtf8(::alcedo::SemanticLabelDisplayText(value, display_language).c_str());
  };
  // top_scores_json_ is already elbow-truncated at assignment time, so it holds exactly
  // the tags worth showing for this image (a single entry when top-1 dominates, several
  // when the score distribution supports them). Display them as-is rather than
  // re-filtering against a fixed margin here.
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(label->top_scores_json_));
  if (!doc.isArray()) {
    return display_label(label->label_);
  }
  const auto  array = doc.array();
  QStringList labels;
  for (const auto& value : array) {
    const auto object = value.toObject();
    const auto name   = object.value(QStringLiteral("label")).toString();
    if (name.isEmpty()) {
      continue;
    }
    labels.push_back(display_label(name.toStdString()));
    if (labels.size() >= static_cast<qsizetype>(kMaxSemanticImageLabelCount)) {
      break;
    }
  }
  return labels.isEmpty() ? display_label(label->label_) : labels.join(QStringLiteral(", "));
}

void SemanticGenerationController::StartGenerationForItems(
    std::vector<SemanticGenerationItem> items, bool forceRegenerate) {
  if (running_) {
    return;
  }
  if (items.empty()) {
    prompt_pending_ = false;
    status_text_    = PL_TEXT("No images are waiting for semantic generation.");
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  pending_items_  = std::move(items);
  prompt_pending_ = false;
  running_        = true;
  embedded_       = 0;
  skipped_        = 0;
  failed_         = 0;
  canceled_       = 0;
  total_          = 0;
  status_text_    = PL_TEXT("Preparing semantic generation...");
  status_->SetTaskState(status_text_, 0, true);
  emit                                   StateChanged();

  QPointer<SemanticGenerationController> self(this);
  QTimer::singleShot(160, this, [self, forceRegenerate]() {
    if (self) {
      self->ContinueGenerationForItems(forceRegenerate);
    }
  });
}

void SemanticGenerationController::ContinueGenerationForItems(bool forceRegenerate) {
  if (!running_) {
    return;
  }

  auto project           = project_->handler().project();
  auto thumbnail_service = project_->handler().thumbnail_service();
  if (!project || !thumbnail_service) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic generation is unavailable without an open project.");
    status_->SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  auto runtime = project->GetAiSidecarRuntimeService();
  if (!runtime) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic runtime service is unavailable.");
    status_->SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  sidecar_lease_ = runtime->AcquireLease();
  if (!sidecar_lease_) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic runtime service is unavailable.");
    status_->SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  AiSidecarRuntimeOptions runtime_options = RuntimeOptionsForCurrentSidecarSnapshot(true);
  auto                    runtime_status  = runtime->Status();
  if (runtime_status.state != AiSidecarRuntimeState::kReady ||
      !runtime_status.model_info.has_value()) {
    status_text_ = PL_TEXT("Starting semantic runtime...");
    emit StateChanged();
    // Register the run as a background task BEFORE the boot so the task bar
    // mirrors the sidecar startup as part of the job and the user can cancel
    // mid-boot. The interactive boot pumps the Qt event loop, keeping the UI
    // responsive while the cold sidecar comes up (a sync StartAndWait here
    // would freeze the UI for up to the 60s startup timeout).
    if (background_task_id_.isEmpty()) {
      background_task_id_ = RegisterBackgroundTask();
    }
    start_canceled_ = false;
    if (!runtime->StartAndWaitInteractive(runtime_options)) {
      running_ = false;
      pending_items_.clear();
      sidecar_lease_.reset();
      if (start_canceled_) {
        start_canceled_ = false;
        status_text_ = PL_TEXT("Semantic generation canceled.");
        if (!background_task_id_.isEmpty()) {
          background_tasks_->FinishTask(background_task_id_,
                                               BackgroundTaskState::Canceled,
                                               status_text_.Render());
          background_task_id_.clear();
        }
      } else {
        runtime_status        = runtime->Status();
        const QString message = QString::fromStdString(runtime_status.message);
        status_text_ = message.isEmpty()
                           ? PL_TEXT("Semantic runtime failed to start.")
                           : PL_TEXT("Semantic runtime failed to start: %1", message);
        if (!background_task_id_.isEmpty()) {
          background_tasks_->FinishTask(background_task_id_,
                                               BackgroundTaskState::Failed,
                                               status_text_.Render());
          background_task_id_.clear();
        }
      }
      status_->SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }

    runtime_status = runtime->Status();
    if (runtime_status.state != AiSidecarRuntimeState::kReady ||
        !runtime_status.model_info.has_value()) {
      const QString message = QString::fromStdString(runtime_status.message);
      running_              = false;
      pending_items_.clear();
      sidecar_lease_.reset();
      status_text_ = message.isEmpty()
                         ? PL_TEXT("Semantic runtime did not report model information.")
                         : PL_TEXT("Semantic runtime is not ready: %1", message);
      if (!background_task_id_.isEmpty()) {
        background_tasks_->FinishTask(background_task_id_,
                                             BackgroundTaskState::Failed,
                                             status_text_.Render());
        background_task_id_.clear();
      }
      status_->SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }
  }

  auto&             semantic       = project->GetStorageService()->GetSemanticStorageController();
  const std::string model_key      = SemanticModelKeyFromInfo(*runtime_status.model_info);
  const auto        label_language = ModelLabelLanguage(*runtime_status.model_info);
  std::string       error;
  if (!semantic.UpsertModel(
          SemanticModelRecord{
              .model_key_     = model_key,
              .model_id_      = runtime_status.model_info->model_id,
              .revision_      = runtime_status.model_info->revision,
              .embedding_dim_ = static_cast<int>(runtime_status.model_info->embedding_dimension),
              .image_size_    = static_cast<int>(runtime_status.model_info->image_size),
              .engine_id_     = runtime_status.model_info->provider,
              .profile_id_    = runtime_status.model_info->profile_id,
              .supported_text_languages_json_ = SemanticSupportedTextLanguagesJson(label_language),
              .prompt_config_hash_            = SemanticPromptConfigHashForLanguage(label_language),
              .active_                        = true},
          &error)) {
    running_ = false;
    pending_items_.clear();
    sidecar_lease_.reset();
    status_text_ =
        PL_TEXT("Semantic model registration failed: %1", QString::fromUtf8(error.c_str()));
    if (!background_task_id_.isEmpty()) {
      background_tasks_->FinishTask(background_task_id_, BackgroundTaskState::Failed,
                                           status_text_.Render());
      background_task_id_.clear();
    }
    status_->SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  pending_items_ =
      ItemsNeedingSemanticGeneration(pending_items_, semantic, model_key, forceRegenerate);
  model_key_ = model_key;
  total_     = ClampToInt(pending_items_.size());
  embedded_  = 0;
  skipped_   = 0;
  failed_    = 0;
  canceled_  = 0;
  if (pending_items_.empty()) {
    running_ = false;
    sidecar_lease_.reset();
    status_text_ = PL_TEXT("All images already have semantic labels.");
    if (!background_task_id_.isEmpty()) {
      background_tasks_->FinishTask(background_task_id_, BackgroundTaskState::Succeeded,
                                           status_text_.Render());
      background_task_id_.clear();
    }
    status_->SetTaskState(status_text_, 100, false);
    status_->ScheduleIdleTaskStateReset(1800);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  status_text_ = PL_TEXT("Generating semantic labels for %1 image(s)...", total_);
  emit                      StateChanged();

  SemanticGenerationOptions options;
  options.thumbnail_resolution = ThumbnailResolution::k256;
  options.thumbnail_batch_size = 8;
  options.embedding_batch_size = EmbeddingBatchSizeForProfile(*runtime_status.model_info);
  options.embedding_timeout    = EmbeddingTimeoutForProfile(*runtime_status.model_info);
  options.expected_model_info  = runtime_status.model_info;
  options.force_regenerate     = forceRegenerate;
  SemanticGenerationPersistenceOptions persistence;
  persistence.storage_controller = &semantic;
  persistence.model_key          = model_key;
  persistence.prompt_config_hash = SemanticPromptConfigHashForLanguage(label_language);
  persistence.label_prototype_batch_size =
      LabelPrototypeBatchSizeForProfile(*runtime_status.model_info);
  options.persistence = persistence;

  auto thumbnails = std::make_shared<ThumbnailServiceSemanticThumbnailProvider>(thumbnail_service);
  auto embedder   = std::make_shared<AiSidecarRuntimeImageEmbeddingClient>(runtime);
  auto service    = std::make_shared<SemanticGenerationService>(thumbnails, embedder);
  QPointer<SemanticGenerationController> self(this);
  auto                                   job = service->StartGeneration(
      pending_items_, options,
      [self](const SemanticGenerationProgress& progress) {
        if (!self) {
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, progress]() {
              if (self) {
                self->UpdateProgress(progress);
              }
            },
            Qt::QueuedConnection);
      },
      [self](std::vector<SemanticGenerationItemResult> results) {
        if (!self) {
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self, results = std::move(results)]() mutable {
              if (self) {
                self->Finish(std::move(results));
              }
            },
            Qt::QueuedConnection);
      });

  job_                = std::move(job);
  // Register the run as a background task now if it wasn't already registered
  // before the sidecar boot (the no-boot path — runtime was already ready).
  if (background_task_id_.isEmpty()) {
    background_task_id_ = RegisterBackgroundTask();
  }
}

auto SemanticGenerationController::RuntimeOptionsForProfile(const QString& profileId,
                                                            bool           profileRoot) const
    -> AiSidecarRuntimeOptions {
  const auto* profile = FindSemanticProfile(profileId.toStdString());
  if (profile == nullptr) {
    profile = &SemanticModelProfiles().front();
  }
  AiSidecarRuntimeOptions options;
  options.model_root =
      profileRoot ? model_download_->ModelRootForProfile(profileId)
                  : QStringToPath(model_download_->ModelDownloadDirectory());
  options.model_id    = profile->model_id;
  options.revision    = profile->revision;
  options.hf_endpoint = model_download_->EffectiveModelEndpoint().toStdString();
  options.provider_config_dir =
      ai_profiles_ ? ai_profiles_->SidecarConfigDir() : std::filesystem::path{};
  options.allow_download      = false;
  options.require_model_info  = profileRoot;
  options.startup_timeout     = kAiSidecarRuntimeStartupTimeout;
  return options;
}

void SemanticGenerationController::UpdateProgress(const SemanticGenerationProgress& progress) {
  total_              = static_cast<int>(progress.total);
  embedded_           = static_cast<int>(progress.embedded);
  skipped_            = static_cast<int>(progress.skipped);
  failed_             = static_cast<int>(progress.failed);
  canceled_           = static_cast<int>(progress.canceled);
  const int completed = embedded_ + skipped_ + failed_ + canceled_;
  const int pct       = total_ > 0 ? (completed * 100) / total_ : 0;
  status_text_ =
      PL_TEXT("Generating semantic labels... %1/%2 complete", completed, std::max(total_, 1));
  status_->SetTaskState(status_text_, pct, true);
  RefreshAlbumSummary();
  if (!background_task_id_.isEmpty()) {
    background_tasks_->UpdateTask(background_task_id_, status_text_.Render(), QString(),
                                         pct);
  }
  emit StateChanged();
}

void SemanticGenerationController::Finish(std::vector<SemanticGenerationItemResult> results) {
  running_ = false;
  job_.reset();
  pending_items_.clear();
  prompt_pending_ = false;

  int embedded    = 0;
  int skipped     = 0;
  int failed      = 0;
  int canceled    = 0;
  for (const auto& result : results) {
    switch (result.status) {
      case SemanticGenerationItemStatus::kEmbedded:
        ++embedded;
        break;
      case SemanticGenerationItemStatus::kSkipped:
        ++skipped;
        break;
      case SemanticGenerationItemStatus::kCanceled:
        ++canceled;
        break;
      case SemanticGenerationItemStatus::kError:
        ++failed;
        break;
      default:
        break;
    }
  }
  embedded_    = embedded;
  skipped_     = skipped;
  failed_      = failed;
  canceled_    = canceled;
  total_       = static_cast<int>(results.size());

  status_text_ = PL_TEXT("Semantic generation complete: %1 generated, %2 skipped, %3 failed.",
                         embedded, skipped, failed + canceled);
  status_->SetTaskState(status_text_, 100, false);
  status_->ScheduleIdleTaskStateReset(2200);
  RefreshAlbumSummary();
  library_->ReloadCurrentFolder();
  if (project_->handler().PersistCurrentProjectState()) {
    QString ignored_error;
    (void)project_->handler().PackageCurrentProjectFiles(&ignored_error);
  }
  if (!background_task_id_.isEmpty()) {
    const BackgroundTaskState final_state =
        canceled > 0
            ? BackgroundTaskState::Canceled
            : (embedded > 0 ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed);
    background_tasks_->FinishTask(background_task_id_, final_state, status_text_.Render());
    background_task_id_.clear();
  }
  sidecar_lease_.reset();
  emit StateChanged();
}

void SemanticGenerationController::ClearPrompt() {
  pending_items_.clear();
  prompt_pending_ = false;
  ResetCounters();
}

void SemanticGenerationController::ResetCounters() {
  total_    = 0;
  embedded_ = 0;
  skipped_  = 0;
  failed_   = 0;
  canceled_ = 0;
}

auto SemanticGenerationController::BuildAffectedTargets() const -> QVariantList {
  QVariantList out;
  for (const auto& it : pending_items_) {
    QVariantMap m;
    m.insert(QStringLiteral("elementId"), static_cast<uint>(it.element_id));
    m.insert(QStringLiteral("imageId"), static_cast<uint>(it.image_id));
    out.append(m);
  }
  return out;
}

auto SemanticGenerationController::RegisterBackgroundTask() -> QString {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::SemanticGeneration;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = status_text_.Render();
  snapshot.progress_percent_ = 0;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.affected_targets_ = BuildAffectedTargets();
  // Phase 2: generation holds the model + download settings stable (don't
  // delete/swap the model files it is using) and blocks a second run. All three
  // are global locks (element_id_ == 0).
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeSemanticModel, 0,
      PL_TEXT("Semantic generation is running; change the model after it finishes.").Render()});
  snapshot.locks_.push_back(
      InteractionLock{InteractionCapability::RunSemanticGeneration, 0,
                      PL_TEXT("Semantic generation is already running.").Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeModelDownloadSettings, 0,
      PL_TEXT("Semantic generation is running; don't change model files now.").Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeImageAnalysisProvider, 0,
      PL_TEXT("Semantic generation is running; change the analysis provider after it finishes.")
          .Render()});
  QPointer<SemanticGenerationController> self(this);
  return background_tasks_->RegisterTask(snapshot, [self]() {
    if (self) {
      self->CancelGeneration();
    }
  });
}

auto SemanticGenerationController::RegisterActivationTask() -> QString {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::ModelActivation;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = PL_TEXT("Activating model and preparing labels...").Render();
  snapshot.progress_percent_ = -1;     // indeterminate — activation has no progress ticks
  snapshot.cancelable_       = false;  // no cancel path; Phase 5 owns the shutdown wait
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  snapshot.affected_targets_ =
      QVariantList{model_download_->SelectedModelProfileId()};
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeSemanticModel, 0,
      PL_TEXT("A model activation is running; change the model after it finishes.").Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::RunSemanticGeneration, 0,
      PL_TEXT("A model activation is running; wait for it to finish before generating labels.")
          .Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeModelDownloadSettings, 0,
      PL_TEXT("A model activation is running; don't change model files now.").Render()});
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::ChangeImageAnalysisProvider, 0,
      PL_TEXT("A model activation is running; change the analysis provider after it finishes.")
          .Render()});
  // Non-cancelable: no cancel callback. The activation finish callback
  // (hopped back to the UI thread) calls FinishTask on this id.
  return background_tasks_->RegisterTask(snapshot);
}

}  // namespace alcedo::ui

#undef PL_TEXT
