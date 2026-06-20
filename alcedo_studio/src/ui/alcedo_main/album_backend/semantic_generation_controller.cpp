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
#include "ui/alcedo_main/album_backend/album_backend.hpp"
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
constexpr auto   kSemanticRuntimeStartupTimeout         = 60s;
constexpr auto   kJinaClipProfileId                     = "jina-clip-v2-int8-multilingual";
constexpr auto   kSiglip2ProfileId                      = "siglip2-b32-256-multilingual";
constexpr size_t kMobileClipBatchSize                   = 64;
constexpr size_t kJinaClipBatchSize                     = 4;
constexpr size_t kSiglip2BatchSize                      = 8;

auto             SemanticModelKeyFromInfo(const SemanticRuntimeModelInfo& info) -> std::string {
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

auto ModelLabelLanguage(const SemanticRuntimeModelInfo& info) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(info.profile_id.empty() ? info.model_id : info.profile_id,
                                       info.language);
}

auto ModelLabelLanguage(const SemanticResolvedModelManifest& manifest) -> SemanticLabelLanguage {
  return SemanticLabelLanguageForModel(
      manifest.profile_id.empty() ? manifest.model_id : manifest.profile_id, manifest.language);
}

auto EmbeddingBatchSizeForProfile(const SemanticRuntimeModelInfo& info) -> size_t {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return kJinaClipBatchSize;
  }
  if (profile_id == kSiglip2ProfileId) {
    return kSiglip2BatchSize;
  }
  return kMobileClipBatchSize;
}

auto LabelPrototypeBatchSizeForProfile(const SemanticRuntimeModelInfo& info) -> size_t {
  return EmbeddingBatchSizeForProfile(info);
}

auto LabelPrototypeBatchSizeForProfile(const SemanticResolvedModelManifest& manifest) -> size_t {
  const SemanticRuntimeModelInfo info{.profile_id = manifest.profile_id,
                                      .model_id   = manifest.model_id};
  return LabelPrototypeBatchSizeForProfile(info);
}

auto EmbeddingTimeoutForProfile(const SemanticRuntimeModelInfo& info) -> std::chrono::milliseconds {
  const auto profile_id = info.profile_id.empty() ? info.model_id : info.profile_id;
  if (profile_id == kJinaClipProfileId) {
    return 120s;
  }
  if (profile_id == kSiglip2ProfileId) {
    return 60s;
  }
  return 30s;
}

auto EmbeddingTimeoutForProfile(const SemanticResolvedModelManifest& manifest)
    -> std::chrono::milliseconds {
  const SemanticRuntimeModelInfo info{.profile_id = manifest.profile_id,
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

class SemanticRuntimeSessionGuard final {
 public:
  explicit SemanticRuntimeSessionGuard(std::shared_ptr<SemanticRuntimeService> runtime)
      : runtime_(std::move(runtime)) {}

  ~SemanticRuntimeSessionGuard() {
    if (runtime_) {
      runtime_->Stop();
    }
  }

  SemanticRuntimeSessionGuard(const SemanticRuntimeSessionGuard&)            = delete;
  SemanticRuntimeSessionGuard& operator=(const SemanticRuntimeSessionGuard&) = delete;

 private:
  std::shared_ptr<SemanticRuntimeService> runtime_;
};

SemanticGenerationController::SemanticGenerationController(AlbumBackend& backend, QObject* parent)
    : QObject(parent), backend_(backend) {
  // The selected-model "active" badge depends on the download controller's
  // install state + selected profile, so recompute it whenever that changes
  // (selection change, download finish, delete, refresh) — not on every
  // progress tick, which is why we listen to SelectedModelInstallChanged.
  connect(&backend_.model_download_controller_, &ModelDownloadController::SelectedModelInstallChanged,
          this, [this]() {
            RecomputeSelectedModelActive();
            emit StateChanged();
          });
}

bool SemanticGenerationController::PromptVisible() const {
  return prompt_pending_ && !running_ && !backend_.nikon_he_recovery_.is_active();
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
  auto project = backend_.project_handler_.project();
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
  auto project = backend_.project_handler_.project();
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

void SemanticGenerationController::RecomputeSelectedModelActive() {
  const QString active_profile = ActiveModelProfileId();
  selected_model_active_ =
      backend_.model_download_controller_.SelectedModelInstalled() && !active_profile.isEmpty()
      && active_profile == backend_.model_download_controller_.SelectedModelProfileId();
}

void SemanticGenerationController::ActivateSelectedModel() {
  if (running_) {
    backend_.model_download_controller_.SetStatusText(
        PL_TEXT("Finish or cancel semantic generation before activating another model."));
    emit StateChanged();
    return;
  }
  if (model_activation_running_) {
    return;
  }
  const QString profile_id = backend_.model_download_controller_.SelectedModelProfileId();

  QString       manifest_error;
  const auto    manifest =
      backend_.model_download_controller_.LoadSelectedResolvedManifest(&manifest_error);
  if (!manifest.has_value()) {
    backend_.model_download_controller_.SetStatusText(
        manifest_error.isEmpty() ? PL_TEXT("Install the selected model before activating it.")
                                 : PL_TEXT("Cannot activate model: %1", manifest_error));
    emit StateChanged();
    return;
  }

  auto project = backend_.project_handler_.project();
  if (!project) {
    backend_.model_download_controller_.SetStatusText(
        PL_TEXT("Open a project before activating a semantic model."));
    emit StateChanged();
    return;
  }
  auto runtime = project->GetSemanticRuntimeService();
  if (!runtime) {
    backend_.model_download_controller_.SetStatusText(
        PL_TEXT("Semantic runtime service is unavailable."));
    emit StateChanged();
    return;
  }

  const std::string model_key      = manifest->revision.empty()
                                         ? manifest->model_id
                                         : manifest->model_id + "@" + manifest->revision;
  const auto        label_language = ModelLabelLanguage(*manifest);

  model_activation_running_ = true;
  backend_.model_download_controller_.SetStatusText(
      PL_TEXT("Activating model and preparing labels..."));
  emit StateChanged();

  auto runtime_options = RuntimeOptionsForProfile(profile_id, true);
  QPointer<SemanticGenerationController> self(this);
  std::thread([self, project = std::move(project), runtime = std::move(runtime),
               manifest = *manifest, model_key, label_language, runtime_options]() mutable {
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
            if (ok) {
              self->model_key_ = model_key;
              self->backend_.model_download_controller_.SetStatusText(
                  prototype_warm
                      ? PL_TEXT("%1 is active for this project. Label prompts are ready.",
                                self->ActiveModelDisplayName())
                      : PL_TEXT("%1 is active for this project.", self->ActiveModelDisplayName()));
              self->RecomputeSelectedModelActive();
              self->RefreshAlbumSummary();
              self->backend_.ReloadCurrentFolder();
            } else {
              const QString detail = QString::fromUtf8(message.c_str());
              self->backend_.model_download_controller_.SetStatusText(
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
      if (runtime_status.state == SemanticRuntimeState::kReady &&
          runtime_status.model_info.has_value() &&
          (runtime_status.model_info->model_id != runtime_options.model_id ||
           runtime_status.model_info->revision != runtime_options.revision ||
           runtime_status.model_info->model_root != runtime_options.model_root.string())) {
        runtime->Stop();
        runtime_status = runtime->Status();
      }
      if (runtime_status.state != SemanticRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        if (!runtime->StartAndWait(runtime_options)) {
          runtime_status = runtime->Status();
          message        = runtime_status.message.empty() ? "semantic runtime failed to start"
                                                          : runtime_status.message;
          finish();
          return;
        }
        runtime_status = runtime->Status();
      }
      if (runtime_status.state != SemanticRuntimeState::kReady ||
          !runtime_status.model_info.has_value()) {
        message = runtime_status.message.empty()
                      ? "semantic runtime did not report model information"
                      : runtime_status.message;
        finish();
        return;
      }

      auto runtime_session = std::make_shared<SemanticRuntimeSessionGuard>(runtime);
      auto embedder        = std::make_shared<SemanticRuntimeImageEmbeddingClient>(runtime);
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
    status_text_ = PL_TEXT("Cancelling semantic generation...");
    emit StateChanged();
  }
}

void SemanticGenerationController::RefreshAlbumSummary() {
  const auto previous_total     = album_total_count_;
  const auto previous_labeled   = album_labeled_count_;
  const auto previous_unlabeled = album_unlabeled_count_;
  const auto previous_summary   = album_summary_text_;

  auto       project            = backend_.project_handler_.project();
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
  auto project = backend_.project_handler_.project();
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
  if (!prompt_pending_ || running_ || backend_.nikon_he_recovery_.is_active()) {
    emit StateChanged();
    return;
  }

  const QString preference =
      NormalizedSemanticPreference(QSettings{}
                                       .value(QLatin1String(kSemanticGenerationImportPreferenceKey),
                                              QLatin1String(kSemanticPreferenceAsk))
                                       .toString());
  if (preference == QLatin1String(kSemanticPreferenceAlways)) {
    StartPendingGeneration(false);
    return;
  }
  if (preference == QLatin1String(kSemanticPreferenceNever)) {
    ClearPrompt();
    return;
  }
  emit StateChanged();
}

auto SemanticGenerationController::StoredModelKey() const -> std::string {
  auto project = backend_.project_handler_.project();
  if (!project) {
    return {};
  }
  return project->GetStorageService()->GetSemanticStorageController().ActiveModelKey();
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
  auto project = backend_.project_handler_.project();
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
  backend_.SetTaskState(status_text_, 0, true);
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

  auto project           = backend_.project_handler_.project();
  auto thumbnail_service = backend_.project_handler_.thumbnail_service();
  if (!project || !thumbnail_service) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic generation is unavailable without an open project.");
    backend_.SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }

  auto runtime = project->GetSemanticRuntimeService();
  if (!runtime) {
    running_ = false;
    pending_items_.clear();
    status_text_ = PL_TEXT("Semantic runtime service is unavailable.");
    backend_.SetTaskState(status_text_, 0, false);
    RefreshAlbumSummary();
    emit StateChanged();
    return;
  }
  QString active_profile_id = ActiveModelProfileId();
  if (active_profile_id.isEmpty()) {
    active_profile_id = backend_.model_download_controller_.SelectedModelProfileId();
  }
  SemanticRuntimeOptions runtime_options = RuntimeOptionsForProfile(active_profile_id, true);
  auto                   runtime_status  = runtime->Status();
  if (runtime_status.state == SemanticRuntimeState::kReady &&
      runtime_status.model_info.has_value() &&
      (runtime_status.model_info->model_id != runtime_options.model_id ||
       runtime_status.model_info->revision != runtime_options.revision ||
       runtime_status.model_info->model_root != runtime_options.model_root.string())) {
    runtime->Stop();
    runtime_status = runtime->Status();
  }
  if (runtime_status.state != SemanticRuntimeState::kReady ||
      !runtime_status.model_info.has_value()) {
    status_text_ = PL_TEXT("Starting semantic runtime...");
    emit StateChanged();

    if (!runtime->StartAndWait(runtime_options)) {
      runtime_status        = runtime->Status();
      const QString message = QString::fromStdString(runtime_status.message);
      running_              = false;
      pending_items_.clear();
      status_text_ = message.isEmpty() ? PL_TEXT("Semantic runtime failed to start.")
                                       : PL_TEXT("Semantic runtime failed to start: %1", message);
      backend_.SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }

    runtime_status = runtime->Status();
    if (runtime_status.state != SemanticRuntimeState::kReady ||
        !runtime_status.model_info.has_value()) {
      const QString message = QString::fromStdString(runtime_status.message);
      running_              = false;
      pending_items_.clear();
      status_text_ = message.isEmpty()
                         ? PL_TEXT("Semantic runtime did not report model information.")
                         : PL_TEXT("Semantic runtime is not ready: %1", message);
      backend_.SetTaskState(status_text_, 0, false);
      RefreshAlbumSummary();
      emit StateChanged();
      return;
    }
  }

  auto              runtime_session = std::make_shared<SemanticRuntimeSessionGuard>(runtime);

  auto&             semantic        = project->GetStorageService()->GetSemanticStorageController();
  const std::string model_key       = SemanticModelKeyFromInfo(*runtime_status.model_info);
  const auto        label_language  = ModelLabelLanguage(*runtime_status.model_info);
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
    status_text_ =
        PL_TEXT("Semantic model registration failed: %1", QString::fromUtf8(error.c_str()));
    backend_.SetTaskState(status_text_, 0, false);
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
    running_     = false;
    status_text_ = PL_TEXT("All images already have semantic labels.");
    backend_.SetTaskState(status_text_, 100, false);
    backend_.ScheduleIdleTaskStateReset(1800);
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
  auto embedder   = std::make_shared<SemanticRuntimeImageEmbeddingClient>(runtime);
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

  runtime_session_ = std::move(runtime_session);
  job_             = std::move(job);
}

auto SemanticGenerationController::RuntimeOptionsForProfile(const QString& profileId,
                                                            bool           profileRoot) const
    -> SemanticRuntimeOptions {
  const auto* profile = FindSemanticProfile(profileId.toStdString());
  if (profile == nullptr) {
    profile = &SemanticModelProfiles().front();
  }
  SemanticRuntimeOptions options;
  options.model_root =
      profileRoot ? backend_.model_download_controller_.ModelRootForProfile(profileId)
                  : QStringToPath(backend_.model_download_controller_.ModelDownloadDirectory());
  options.model_id           = profile->model_id;
  options.revision           = profile->revision;
  options.hf_endpoint        = backend_.model_download_controller_.EffectiveModelEndpoint().toStdString();
  options.allow_download     = false;
  options.require_model_info = profileRoot;
  options.startup_timeout    = kSemanticRuntimeStartupTimeout;
  return options;
}

void SemanticGenerationController::UpdateProgress(const SemanticGenerationProgress& progress) {
  total_              = static_cast<int>(progress.total);
  embedded_           = static_cast<int>(progress.embedded);
  skipped_            = static_cast<int>(progress.skipped);
  failed_             = static_cast<int>(progress.failed);
  canceled_           = static_cast<int>(progress.canceled);
  const int completed = embedded_ + skipped_ + failed_ + canceled_;
  status_text_ =
      PL_TEXT("Generating semantic labels... %1/%2 complete", completed, std::max(total_, 1));
  backend_.SetTaskState(status_text_, total_ > 0 ? (completed * 100) / total_ : 0, true);
  RefreshAlbumSummary();
  emit StateChanged();
}

void SemanticGenerationController::Finish(std::vector<SemanticGenerationItemResult> results) {
  running_ = false;
  job_.reset();
  runtime_session_.reset();
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
  backend_.SetTaskState(status_text_, 100, false);
  backend_.ScheduleIdleTaskStateReset(2200);
  RefreshAlbumSummary();
  backend_.ReloadCurrentFolder();
  if (backend_.project_handler_.PersistCurrentProjectState()) {
    QString ignored_error;
    (void)backend_.project_handler_.PackageCurrentProjectFiles(&ignored_error);
  }
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

}  // namespace alcedo::ui

#undef PL_TEXT
