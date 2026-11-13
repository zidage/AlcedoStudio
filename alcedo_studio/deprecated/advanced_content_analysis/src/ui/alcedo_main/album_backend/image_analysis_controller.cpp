//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"

#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QVariantMap>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

using namespace std::chrono_literals;

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {

// Remote analysis jobs may span many serialized provider calls. Use the sidecar
// default credential TTL (currently one hour) and still revoke explicitly when
// the job finishes; a short host TTL can expire mid-run on slow batch analysis.
constexpr int64_t kAnalysisCredentialTtlMs = 0;

void              ClearSecret(std::string* secret) {
  if (secret == nullptr) {
    return;
  }
  if (!secret->empty()) {
    std::fill(secret->begin(), secret->end(), '0');
  }
  secret->clear();
  secret->shrink_to_fit();
}

// Resolve the profile output_language to a concrete code the sidecar accepts.
// "follow" (the default) resolves to the current app language — reading
// QSettings("ui/language") and resolving "system" via QLocale, the same pattern
// as semantic_generation_controller.cpp's CurrentUiSemanticLabelLanguage. The
// sidecar only understands "" / "en" / "zh", so "follow" never crosses the
// wire. Returns "" (English default) for any unknown value.
bool ProfileSupportsStructuredImageAnalysis(const alcedo::AiProviderProfile& profile) {
  return !profile.model_id.isEmpty() && profile.structured_output_mode != QStringLiteral("none");
}
QString ResolveOutputLanguage(const QString& preference) {
  const QString v = preference.trimmed().toLower();
  if (v == "en") {
    return QStringLiteral("en");
  }
  if (v == "zh") {
    return QStringLiteral("zh");
  }
  // "follow" (or anything else) -> current app language.
  QString code =
      QSettings().value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
  if (code.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
    code = QLocale::system().bcp47Name();
  }
  return code.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive) ? QStringLiteral("zh")
                                                                    : QStringLiteral("en");
}

// Clamp a host-supplied rating-severity code to one of the five personas the
// sidecar understands. "" / "normal" / any unrecognized value -> "normal"
// (fail open to the balanced default, matching the Rust
// `normalize_rating_severity`). QSettings key: ai/analysis/ratingSeverity.
QString NormalizeRatingSeverity(const QString& value) {
  const QString v = value.trimmed().toLower();
  if (v == QLatin1String("lite")) {
    return QStringLiteral("lite");
  }
  if (v == QLatin1String("high")) {
    return QStringLiteral("high");
  }
  if (v == QLatin1String("xhigh") || v == QLatin1String("x_high")) {
    return QStringLiteral("xhigh");
  }
  if (v == QLatin1String("max")) {
    return QStringLiteral("max");
  }
  return QStringLiteral("normal");
}

bool TaskIncludesRating(alcedo::ImageAnalysisTask task) {
  return task == alcedo::ImageAnalysisTask::kScore || task == alcedo::ImageAnalysisTask::kAnalyze;
}

bool ShouldAttachCameraContext(alcedo::ImageAnalysisTask task, const QString& rating_severity) {
  return TaskIncludesRating(task) &&
         (rating_severity == QStringLiteral("xhigh") || rating_severity == QStringLiteral("max"));
}

constexpr const char* kRatingSeveritySettingsKey = "ai/analysis/ratingSeverity";

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// ImageAnalysisController
// ────────────────────────────────────────────────────────────────────────────
ImageAnalysisController::ImageAnalysisController(std::shared_ptr<IImageAnalysisEnvironment> env,
                                                 AiProviderProfileController*        profiles,
                                                 std::shared_ptr<IImageAnalysisSink> sink,
                                                 BackgroundTaskController*           registry,
                                                 QObject*                            parent)
    : QObject(parent),
      env_(std::move(env)),
      profiles_(profiles),
      sink_(std::move(sink)),
      registry_(registry) {
  rating_severity_ = NormalizeRatingSeverity(
      QSettings()
          .value(QLatin1String(kRatingSeveritySettingsKey), QStringLiteral("normal"))
          .toString());
  if (profiles_) {
    connect(profiles_, &AiProviderProfileController::ProfilesChanged, this,
            [this] { RefreshConfiguredState(); });
  }
  RefreshConfiguredState();
}

void ImageAnalysisController::RefreshConfiguredState() {
  if (!profiles_) {
    provider_configured_  = false;
    credential_available_ = false;
    emit StateChanged();
    return;
  }
  const auto profile                  = profiles_->ActiveProfile();
  const bool was_provider_configured  = provider_configured_;
  const bool was_credential_available = credential_available_;
  provider_configured_ =
      profile.has_value() && !profile->provider_id.isEmpty() && !profile->model_id.isEmpty();
  if (profile && profile->auth_type == QStringLiteral("none")) {
    credential_available_ = true;
  } else if (profile && !profile->credential_slot.isEmpty()) {
    auto store = profiles_->CredentialStore();
    if (!store && env_) {
      store = env_->CredentialStore();
    }
    credential_available_ = store && store->HasCredential(profile->credential_slot.toStdString());
  } else {
    credential_available_ = false;
  }
  if (was_provider_configured != provider_configured_ ||
      was_credential_available != credential_available_) {
    emit StateChanged();
  }
}

void ImageAnalysisController::RefreshCredentialState() { RefreshConfiguredState(); }

bool ImageAnalysisController::SetRatingSeverity(const QString& value) {
  const QString normalized = NormalizeRatingSeverity(value);
  if (normalized == rating_severity_) {
    return true;
  }
  rating_severity_ = normalized;
  QSettings().setValue(QLatin1String(kRatingSeveritySettingsKey), normalized);
  emit StateChanged();
  return true;
}
auto ImageAnalysisController::CollectItems(const QVariantList& targetEntries)
    -> std::vector<alcedo::ImageAnalysisItem> {
  std::vector<alcedo::ImageAnalysisItem> items;
  std::unordered_set<uint64_t>           seen;
  for (const QVariant& entry : targetEntries) {
    const auto map       = entry.toMap();
    const auto elementId = static_cast<sl_element_id_t>(map.value("elementId").toUInt());
    const auto imageId   = static_cast<image_id_t>(map.value("imageId").toUInt());
    if (elementId == 0 || imageId == 0) {
      continue;
    }
    const uint64_t key = (static_cast<uint64_t>(elementId) << 32) | static_cast<uint64_t>(imageId);
    if (!seen.insert(key).second) {
      continue;
    }
    items.push_back(alcedo::ImageAnalysisItem{elementId, imageId});
  }
  return items;
}

void ImageAnalysisController::ResetCounters() {
  total_    = 0;
  analyzed_ = 0;
  failed_   = 0;
  canceled_ = 0;
}

auto ImageAnalysisController::BuildAffectedTargets() const -> QVariantList {
  QVariantList out;
  for (const auto& it : last_items_) {
    QVariantMap m;
    m.insert(QStringLiteral("elementId"), static_cast<uint>(it.element_id));
    m.insert(QStringLiteral("imageId"), static_cast<uint>(it.image_id));
    out.append(m);
  }
  return out;
}

auto ImageAnalysisController::RegisterBackgroundTask() -> QString {
  if (!registry_) {
    return {};
  }
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::ImageAnalysis;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = status_text_.Render();
  snapshot.progress_percent_ = 0;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.affected_targets_ = BuildAffectedTargets();
  // Phase 2: publish interaction locks so the policy controller disables only
  // the controls that conflict with this run. Image-field locks are per-element
  // (so the inspector for an unrelated image stays editable); the provider-change
  // lock is global. `EditImageRatingReason` is published only when this run will
  // actually write reasons. `EditImageRating` is published even for a describe
  // job (conservative; the plan's acceptance disables rating for affected images).
  const QString edit_reason  = Tr("This image is being analyzed.");
  const QString delete_reason =
      Tr("This image is being analyzed and cannot be deleted or removed.");
  const QString rerun_reason = Tr("An analysis is already running for this image.");
  for (const auto& item : last_items_) {
    const quint64 eid = static_cast<quint64>(item.element_id);
    snapshot.locks_.push_back(
        InteractionLock{InteractionCapability::EditImageDescription, eid, edit_reason});
    snapshot.locks_.push_back(
        InteractionLock{InteractionCapability::EditImageRating, eid, edit_reason});
    snapshot.locks_.push_back(
        InteractionLock{InteractionCapability::EditImageRatingReason, eid, edit_reason});
    snapshot.locks_.push_back(
        InteractionLock{InteractionCapability::RunImageAnalysis, eid, rerun_reason});
    snapshot.locks_.push_back(
        InteractionLock{InteractionCapability::DeleteImages, eid, delete_reason});
  }
  snapshot.locks_.push_back(
      InteractionLock{InteractionCapability::ChangeImageAnalysisProvider, 0,
                      Tr("An analysis is running; change the provider after it finishes.")});
  snapshot.locks_.push_back(
      InteractionLock{InteractionCapability::ChangeSemanticModel, 0,
                      Tr("An analysis is running; change the local AI model after it finishes.")});
  snapshot.locks_.push_back(
      InteractionLock{InteractionCapability::ChangeModelDownloadSettings, 0,
                      Tr("An analysis is running; don't change model files now.")});
  QPointer<ImageAnalysisController> self(this);
  return registry_->RegisterTask(snapshot, [self]() {
    if (self) {
      self->CancelAnalysis();
    }
  });
}

void ImageAnalysisController::SetError(const QString& error) {
  last_error_  = error;
  status_text_ = i18n::LocalizedText{};
  running_     = false;
  can_retry_   = false;
  job_.reset();
  sidecar_lease_.reset();
  last_items_.clear();
  last_results_.clear();
  last_usage_.clear();
  ResetCounters();
  if (registry_ && !background_task_id_.isEmpty()) {
    registry_->FinishTask(background_task_id_, BackgroundTaskState::Failed, error);
    background_task_id_.clear();
  }
  emit StateChanged();
}

void ImageAnalysisController::StartDescribeForTargets(const QVariantList& targetEntries) {
  StartForTargets(targetEntries, alcedo::ImageAnalysisTask::kDescribe, false);
}

void ImageAnalysisController::StartScoreForTargets(const QVariantList& targetEntries,
                                                   bool                includeRatingReasons) {
  StartForTargets(targetEntries, alcedo::ImageAnalysisTask::kScore, includeRatingReasons);
}

void ImageAnalysisController::StartAnalyzeForTargets(const QVariantList& targetEntries,
                                                     bool                includeRatingReasons) {
  StartForTargets(targetEntries, alcedo::ImageAnalysisTask::kAnalyze, includeRatingReasons);
}

void ImageAnalysisController::StartForTargets(const QVariantList&       targetEntries,
                                              alcedo::ImageAnalysisTask task,
                                              bool                      includeRatingReasons) {
  if (running_) {
    return;
  }
  last_error_.clear();
  start_canceled_  = false;
  last_task_       = task;
  last_include_rating_reasons_ =
      task == alcedo::ImageAnalysisTask::kDescribe ? false : includeRatingReasons;

  auto items = CollectItems(targetEntries);
  if (items.empty()) {
    SetError(Tr("Select at least one image to analyze."));
    return;
  }

  if (env_ && ShouldAttachCameraContext(task, rating_severity_)) {
    for (auto& item : items) {
      item.camera_context = env_->CameraContextForItem(item);
    }
  }

  if (!profiles_) {
    SetError(Tr("No provider profile is configured."));
    return;
  }
  const auto profile_opt = profiles_->ActiveProfile();
  if (!profile_opt) {
    provider_configured_ = false;
    SetError(Tr("Add and use an Advanced Content Analysis provider before analyzing images."));
    return;
  }
  const auto profile = *profile_opt;
  if (profile.provider_id.isEmpty() || profile.model_id.isEmpty()) {
    provider_configured_ = false;
    SetError(Tr("Configure a provider and model before analyzing images."));
    return;
  }
  provider_configured_ = true;
  if (!ProfileSupportsStructuredImageAnalysis(profile)) {
    SetError(
        Tr("Selected provider is not configured for structured output. Choose a "
           "structured-output capable Advanced Content Analysis provider."));
    return;
  }

  std::string secret;
  if (profile.auth_type != QStringLiteral("none")) {
    std::string cred_err;
    auto        store = profiles_->CredentialStore();
    if (!store && env_) {
      store = env_->CredentialStore();
    }
    if (!store ||
        !store->LoadCredential(profile.credential_slot.toStdString(), &secret, &cred_err)) {
      credential_available_ = false;
      SetError(Tr("No API key stored for credential slot '%1'. Save a key first.")
                   .arg(profile.credential_slot));
      return;
    }
  }
  credential_available_              = true;

  const bool  provider_configs_dirty = profiles_->SidecarConfigsDirty();
  std::string config_err;
  if (!profiles_->PrepareSidecarConfigDir(&config_err)) {
    ClearSecret(&secret);
    SetError(Tr("Could not write provider configs: %1").arg(QString::fromStdString(config_err)));
    return;
  }

  sidecar_lease_ = env_->AcquireSidecarLease();
  if (!sidecar_lease_) {
    ClearSecret(&secret);
    SetError(Tr("AI sidecar runtime is unavailable. Open a project first."));
    return;
  }

  // Register the run as a background task and surface "Starting AI sidecar..."
  // BEFORE the boot so the task bar mirrors the sidecar startup as part of the
  // job and the UI stays interactive (the interactive boot pumps the event
  // loop). The boot can take several seconds on a cold sidecar; doing this
  // first means the user sees live status and can cancel mid-boot.
  ResetCounters();
  total_      = static_cast<int>(items.size());
  running_    = true;
  can_retry_  = false;
  last_items_ = items;
  status_text_ = PL_TEXT("Starting AI sidecar...");
  emit StateChanged();
  if (registry_) {
    background_task_id_ = RegisterBackgroundTask();
  }

  std::string sidecar_err;
  if (!env_->EnsureSidecarReadyInteractive(provider_configs_dirty, &sidecar_err)) {
    ClearSecret(&secret);
    sidecar_lease_.reset();
    if (start_canceled_) {
      // User pressed Cancel during the boot — finish as Canceled, not Failed.
      start_canceled_ = false;
      running_    = false;
      can_retry_  = false;
      canceled_   = total_;
      status_text_ = PL_TEXT("Canceled.");
      if (registry_ && !background_task_id_.isEmpty()) {
        registry_->FinishTask(background_task_id_, BackgroundTaskState::Canceled,
                              status_text_.Render());
        background_task_id_.clear();
      }
      emit StateChanged();
    } else {
      SetError(Tr("Could not start the AI sidecar: %1").arg(QString::fromStdString(sidecar_err)));
    }
    return;
  }

  auto thumbnail_provider = env_->ThumbnailProvider();
  auto analysis_client    = env_->AnalysisClient();
  auto concurrency_limit  = env_->ConcurrencyLimit();
  if (!thumbnail_provider || !analysis_client || !concurrency_limit) {
    ClearSecret(&secret);
    sidecar_lease_.reset();
    SetError(Tr("Image analysis runtime is unavailable. Open a project first."));
    return;
  }

  alcedo::ImageAnalysisOptions options;
  options.task                   = task;
  options.thumbnail_resolution   = alcedo::ThumbnailResolution::k1024;
  options.jpeg_quality           = 90;
  options.timeout                = std::chrono::milliseconds(profile.timeout_ms);
  options.provider_id            = profile.provider_id.toStdString();
  options.model_id               = profile.model_id.toStdString();
  options.output_language        = ResolveOutputLanguage(profiles_->OutputLanguage()).toStdString();
  options.rating_severity        = rating_severity_.toStdString();
  options.include_rating_reasons = last_include_rating_reasons_;
  options.credential.provider_id = profile.provider_id.toStdString();
  options.credential.secret      = std::move(secret);
  options.credential_ttl_ms      = kAnalysisCredentialTtlMs;
  options.temp_dir               = std::filesystem::temp_directory_path();
  options.prefetch               = 1;
  options.max_image_bytes        = profile.max_image_bytes;
  if (task == alcedo::ImageAnalysisTask::kScore || task == alcedo::ImageAnalysisTask::kAnalyze) {
    options.rubric_id = "general";
  }
  status_text_ = task == alcedo::ImageAnalysisTask::kDescribe
                     ? PL_TEXT("Analyzing %1 image(s) for captions and tags...", total_)
                 : task == alcedo::ImageAnalysisTask::kAnalyze
                     ? PL_TEXT("Analyzing and scoring %1 image(s)...", total_)
                     : PL_TEXT("Scoring %1 image(s)...", total_);
  emit StateChanged();

  // Build a fresh service per job, passing the SHARED concurrency limit so remote calls
  // serialize app-wide across every controller/service instance.
  alcedo::ImageAnalysisService      service(thumbnail_provider, analysis_client, concurrency_limit);

  QPointer<ImageAnalysisController> self(this);
  auto                              job = service.StartAnalysis(
      std::move(items), std::move(options),
      [self](const alcedo::ImageAnalysisProgress& progress) {
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
      [self](std::vector<alcedo::ImageAnalysisItemResult> results) {
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
  job_ = std::move(job);
}

void ImageAnalysisController::CancelAnalysis() {
  if (!running_) {
    return;
  }
  if (job_) {
    // Job phase: a real analysis job is in flight on the worker thread.
    status_text_ = PL_TEXT("Cancelling...");
    emit StateChanged();
    job_->Cancel();
    if (registry_ && !background_task_id_.isEmpty()) {
      registry_->UpdateTaskState(background_task_id_, BackgroundTaskState::Canceling);
    }
    return;
  }
  // Boot phase: the interactive sidecar boot is still running (job_ is null).
  // Flag the cancel so the boot-failure path finishes as Canceled, and ask the
  // runtime to abort at its next poll checkpoint.
  start_canceled_ = true;
  if (env_) {
    env_->RequestSidecarStartCancel();
  }
  status_text_ = PL_TEXT("Cancelling...");
  emit StateChanged();
  if (registry_ && !background_task_id_.isEmpty()) {
    registry_->UpdateTaskState(background_task_id_, BackgroundTaskState::Canceling);
  }
}

void ImageAnalysisController::RetryLast() {
  if (running_ || last_items_.empty()) {
    return;
  }
  auto         items                = last_items_;
  auto         task                 = last_task_;
  const bool   includeRatingReasons = last_include_rating_reasons_;
  // Re-run via StartForTargets by reconstructing the target entries from items.
  QVariantList entries;
  for (const auto& it : items) {
    QVariantMap m;
    m.insert("elementId", static_cast<uint>(it.element_id));
    m.insert("imageId", static_cast<uint>(it.image_id));
    entries.push_back(m);
  }
  StartForTargets(entries, task, includeRatingReasons);
}

void ImageAnalysisController::ValidateConnection() {
  ValidateConnectionForProfile(profiles_ ? profiles_->ActiveProfileId() : QString{});
}

void ImageAnalysisController::ValidateConnectionForProfile(const QString& profileId) {
  if (running_ || !profiles_ || !env_) {
    return;
  }
  const auto profile_opt = profiles_->ProfileById(profileId);
  if (!profile_opt) {
    SetError(Tr("Select a provider profile before validating."));
    return;
  }
  const auto profile             = *profile_opt;
  const bool requires_credential = profile.auth_type != QStringLiteral("none");
  if (profile.provider_id.isEmpty() || (requires_credential && profile.credential_slot.isEmpty())) {
    SetError(Tr("Configure a provider id and credential slot before validating."));
    return;
  }

  auto store = profiles_->CredentialStore();
  if (!store) {
    store = env_->CredentialStore();
  }
  if (requires_credential && !store) {
    SetError(Tr("Image analysis runtime is unavailable."));
    return;
  }
  std::string slot = profile.credential_slot.toStdString();
  if (requires_credential && !store->HasCredential(slot)) {
    credential_available_ = false;
    SetError(Tr("No credential stored for credential slot '%1'. Connect or save credentials first.")
                 .arg(profile.credential_slot));
    return;
  }
  credential_available_              = true;

  const bool  provider_configs_dirty = profiles_->SidecarConfigsDirty();
  std::string config_err;
  if (!profiles_->PrepareSidecarConfigDir(&config_err)) {
    SetError(Tr("Could not write provider configs: %1").arg(QString::fromStdString(config_err)));
    return;
  }

  std::string sidecar_err;
  auto        sidecar_lease = env_->AcquireSidecarLease();
  if (!sidecar_lease) {
    SetError(Tr("AI sidecar runtime is unavailable. Open a project first."));
    return;
  }
  // Interactive boot on the UI thread: pumps the Qt event loop during the
  // sidecar readiness poll so the "Test" button doesn't freeze the UI while a
  // cold sidecar comes up.
  if (!env_->EnsureSidecarReadyInteractive(provider_configs_dirty, &sidecar_err)) {
    SetError(Tr("Could not start the AI sidecar: %1").arg(QString::fromStdString(sidecar_err)));
    return;
  }
  auto thumbnail_provider = env_->ThumbnailProvider();
  auto analysis_client    = env_->AnalysisClient();
  auto concurrency_limit  = env_->ConcurrencyLimit();
  if (!thumbnail_provider || !analysis_client || !concurrency_limit) {
    SetError(Tr("Image analysis runtime is unavailable."));
    return;
  }

  std::string                       provider_id = profile.provider_id.toStdString();
  int64_t                           timeout_ms  = profile.timeout_ms;
  const QString                     profile_id  = profile.uuid;
  QPointer<ImageAnalysisController> self(this);
  std::thread([self, provider_id, profile_id, slot, timeout_ms, requires_credential,
               thumbnail_provider, analysis_client, concurrency_limit, store,
               sidecar_lease = std::move(sidecar_lease)]() {
    alcedo::ImageAnalysisService service(thumbnail_provider, analysis_client, concurrency_limit);
    alcedo::ImageAnalysisConnectionValidationOptions opts;
    opts.provider_id         = provider_id;
    opts.credential_slot     = slot;
    opts.requires_credential = requires_credential;
    opts.timeout             = std::chrono::milliseconds(timeout_ms);
    opts.credential_ttl_ms   = 60000;
    auto result              = service.ValidateConnection(opts, store.get());
    QMetaObject::invokeMethod(
        self,
        [self, result, profile_id]() {
          if (!self) {
            return;
          }
          if (result.ok) {
            self->last_error_.clear();
            QVariantList models;
            models.reserve(static_cast<int>(result.models.size()));
            for (const auto& m : result.models) {
              QVariantMap entry;
              entry.insert(QStringLiteral("modelId"), QString::fromStdString(m.model_id));
              entry.insert(QStringLiteral("displayName"), QString::fromStdString(m.display_name));
              entry.insert(QStringLiteral("sourceProviderId"),
                           QString::fromStdString(m.source_provider_id));
              models.append(entry);
            }
            self->discovered_models_ = models;
            if (self->profiles_) {
              self->profiles_->SetDiscoveredModels(profile_id, models);
            }
            const int n              = static_cast<int>(result.models.size());
            self->status_text_       = PL_TEXT("Connection OK — %1 model(s) visible.", n);
            self->connection_status_ = Tr("Connected — %1 model(s) available.").arg(n);
          } else {
            self->last_error_  = QString::fromStdString(result.error);
            self->status_text_ = i18n::LocalizedText{};
            self->connection_status_ =
                Tr("Connection failed: %1").arg(QString::fromStdString(result.error));
            self->discovered_models_.clear();
          }
          self->RefreshConfiguredState();
          emit self->StateChanged();
        },
        Qt::QueuedConnection);
  }).detach();
}
void ImageAnalysisController::UpdateProgress(const alcedo::ImageAnalysisProgress& progress) {
  total_              = static_cast<int>(progress.total);
  analyzed_           = static_cast<int>(progress.analyzed);
  failed_             = static_cast<int>(progress.failed);
  canceled_           = static_cast<int>(progress.canceled);
  const int completed = analyzed_ + failed_ + canceled_;
  const int pct       = total_ > 0 ? (completed * 100) / total_ : 0;
  status_text_        = (last_task_ == alcedo::ImageAnalysisTask::kDescribe)
                            ? PL_TEXT("Analyzing captions/tags: %1/%2 (%3%)", completed, total_, pct)
                        : (last_task_ == alcedo::ImageAnalysisTask::kAnalyze)
                            ? PL_TEXT("Analyzing/scoring: %1/%2 (%3%)", completed, total_, pct)
                            : PL_TEXT("Scoring: %1/%2 (%3%)", completed, total_, pct);
  if (registry_ && !background_task_id_.isEmpty()) {
    registry_->UpdateTask(background_task_id_, status_text_.Render(), QString(), pct);
  }
  emit StateChanged();
}

void ImageAnalysisController::Finish(std::vector<alcedo::ImageAnalysisItemResult> results) {
  running_ = false;
  ResetCounters();
  last_results_.clear();
  // Phase 7a usage aggregate: token totals across all items + the distinct provider
  // request ids, plus how many items carried usage metadata vs not. Per-item usage is
  // NOT placed in `lastResults` — this aggregate is the 7a summary.
  int64_t      usage_input  = 0;
  int64_t      usage_output = 0;
  int64_t      usage_total  = 0;
  QVariantList usage_request_ids;
  int          items_with_usage    = 0;
  int          items_without_usage = 0;
  const bool   describe            = (last_task_ == alcedo::ImageAnalysisTask::kDescribe);
  const bool   analyze             = (last_task_ == alcedo::ImageAnalysisTask::kAnalyze);

  for (const auto& r : results) {
    QVariantMap m;
    m.insert("elementId", static_cast<uint>(r.item.element_id));
    m.insert("imageId", static_cast<uint>(r.item.image_id));
    m.insert("status", QString::fromUtf8(alcedo::ToString(r.status)));
    m.insert("error", QString::fromStdString(r.error));
    const std::string& provider =
        (describe || analyze) ? r.understanding.provider : r.rating.provider;
    const std::string& model_id =
        (describe || analyze) ? r.understanding.model_id : r.rating.model_id;
    const int provider_status = (describe || analyze) ? r.understanding.status : r.rating.status;
    const int provider_error_code =
        (describe || analyze) ? r.understanding.error_code : r.rating.error_code;
    const std::string& prompt_profile_id =
        (describe || analyze) ? r.understanding.prompt_profile_id : r.rating.prompt_profile_id;
    const std::string& provider_request_id =
        (describe || analyze) ? r.understanding.provider_request_id : r.rating.provider_request_id;
    const auto& usage = (describe || analyze) ? r.understanding.usage : r.rating.usage;
    m.insert("provider", QString::fromStdString(provider));
    m.insert("modelId", QString::fromStdString(model_id));
    m.insert("providerStatus", provider_status);
    m.insert("providerErrorCode", provider_error_code);
    // Identity on every job result so a prompt/model change does not reinterpret old
    // annotations: prompt-profile id + the provider's own request id (provider/modelId
    // already present above).
    m.insert("promptProfileId", QString::fromStdString(prompt_profile_id));
    m.insert("providerRequestId", QString::fromStdString(provider_request_id));

    const bool item_has_usage = (usage.total_tokens != 0 || !provider_request_id.empty());
    if (item_has_usage) {
      ++items_with_usage;
      usage_input += usage.input_tokens;
      usage_output += usage.output_tokens;
      usage_total += usage.total_tokens;
      if (!provider_request_id.empty()) {
        usage_request_ids.push_back(QString::fromStdString(provider_request_id));
      }
    } else {
      ++items_without_usage;
    }

    if (r.status == alcedo::ImageAnalysisItemStatus::kAnalyzed) {
      if (describe || analyze) {
        m.insert("caption", QString::fromStdString(r.understanding.caption));
        QVariantList tags;
        for (const auto& t : r.understanding.tags) {
          tags.push_back(QString::fromStdString(t));
        }
        m.insert("tags", tags);
        m.insert("scene", QString::fromStdString(r.understanding.scene));
      }
      if (!describe) {
        m.insert("rating", r.rating.rating);
        m.insert("rubricId", QString::fromStdString(r.rating.rubric_id));
        m.insert("reasons", QString::fromStdString(r.rating.reasons));
      }
      analyzed_++;
    } else if (r.status == alcedo::ImageAnalysisItemStatus::kCanceled) {
      canceled_++;
    } else {
      failed_++;
    }
    last_results_.push_back(m);
  }
  total_ = static_cast<int>(results.size());

  // Build the usage aggregate (always present, even on an all-failure job, so QML can
  // read itemsWithUsage=0).
  QVariantMap usage_map;
  usage_map.insert("inputTokens", static_cast<qlonglong>(usage_input));
  usage_map.insert("outputTokens", static_cast<qlonglong>(usage_output));
  usage_map.insert("totalTokens", static_cast<qlonglong>(usage_total));
  usage_map.insert("providerRequestIds", usage_request_ids);
  usage_map.insert("itemsWithUsage", items_with_usage);
  usage_map.insert("itemsWithoutUsage", items_without_usage);
  last_usage_            = usage_map;

  const bool any_failure = (failed_ > 0 || canceled_ > 0);
  can_retry_             = any_failure && !last_items_.empty();
  if (analyzed_ == total_ && total_ > 0) {
    last_error_.clear();
    status_text_ = (last_task_ == alcedo::ImageAnalysisTask::kDescribe)
                       ? PL_TEXT("Analyzed %1 image(s).", analyzed_)
                   : (last_task_ == alcedo::ImageAnalysisTask::kAnalyze)
                       ? PL_TEXT("Analyzed and scored %1 image(s).", analyzed_)
                       : PL_TEXT("Scored %1 image(s).", analyzed_);
  } else if (any_failure) {
    status_text_ = PL_TEXT("Done: %1 ok, %2 failed, %3 canceled.", analyzed_, failed_, canceled_);
  } else {
    status_text_ = i18n::LocalizedText{};
  }

  // Phase 7a persistence (job end, in the finished callback). A cancelled or failed run
  // must leave no active annotation: only `kAnalyzed` items reach the sink, and the
  // trailing flush/notify fires only when at least one item was persisted (analyzed_ > 0),
  // so a fully failed/cancelled job produces ZERO sink calls. The sink is nullable so the
  // controller stays usable in contexts without host-state wiring (e.g. a dry unit test).
  if (sink_ && analyzed_ > 0) {
    std::vector<alcedo::ImageAnalysisItemResult> understanding_results;
    if (describe || analyze) {
      understanding_results.reserve(static_cast<size_t>(analyzed_));
    }
    for (const auto& r : results) {
      if (r.status != alcedo::ImageAnalysisItemStatus::kAnalyzed) {
        continue;
      }
      if (describe || analyze) {
        understanding_results.push_back(r);
      }
      if (!describe) {
        if (last_include_rating_reasons_) {
          sink_->PersistRatingReasons(r);
        }
        sink_->ApplyStarRating(static_cast<uint32_t>(r.item.element_id),
                               static_cast<uint32_t>(r.item.image_id), r.rating.rating);
      }
    }
    if (describe || analyze) {
      sink_->PersistUnderstandings(understanding_results);
      sink_->NotifySearchDocumentChanged();
    }
    if (!describe) {
      sink_->FlushPendingStarRatings();
    }
  }

  RefreshConfiguredState();
  // Phase 2 (Step 4): if writes are queued behind the project DB write barrier,
  // keep the task Running (locks held) until they actually commit, so the
  // inspector stays disabled across the gap and a user can't edit a
  // just-analyzed image's rating in the window before the queued AI rating
  // would overwrite it. The drain-complete callback (fired by the barrier's
  // on_release_ → sink FlushPendingWrites → queue Drain) finishes the task then.
  // Otherwise finish now — no barrier, writes already committed (today's path).
  if (registry_ && !background_task_id_.isEmpty()) {
    const BackgroundTaskState final_state =
        canceled_ > 0
            ? BackgroundTaskState::Canceled
            : (analyzed_ > 0 ? BackgroundTaskState::Succeeded : BackgroundTaskState::Failed);
    const QString detail = status_text_.Render();
    if (sink_ && sink_->HasPendingWrites()) {
      // Capture the task id by value — it is independent of the background_task_id_
      // member, which a subsequent job's RegisterBackgroundTask may overwrite. The
      // drain-complete FIFO in the queue handles multiple deferred jobs.
      const QString                     task_id = background_task_id_;
      QPointer<ImageAnalysisController> self(this);
      sink_->SetOnDrainComplete([self, registry = registry_, task_id, final_state, detail] {
        if (self && registry) {
          registry->FinishTask(task_id, final_state, detail);
        }
      });
      // Intentionally do NOT clear background_task_id_ here: the captured task_id
      // is what finishes the task, and the member is overwritten on the next run.
    } else {
      registry_->FinishTask(background_task_id_, final_state, detail);
      background_task_id_.clear();
    }
  }
  sidecar_lease_.reset();
  emit StateChanged();
}

#undef PL_TEXT

}  // namespace alcedo::ui
