//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <string>
#include <vector>

#include "app/ai_credential_store.hpp"
#include "app/ai_provider_profile.hpp"
#include "app/image_analysis_service.hpp"
#include "ui/alcedo_main/album_backend/image_analysis_sink.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class BackgroundTaskController;
class ImageController;
class ProjectDbWriteBarrier;
class ProjectModule;
class SemanticGenerationController;
class StatsEngine;

/// Phase 6d — the runtime seams `ImageAnalysisController` needs, as an interface
/// so the controller is unit-testable without a live project / sidecar.
///
/// The production implementation (`AlbumImageAnalysisEnvironment`, in the .cpp)
/// resolves these lazily from the open project's services at call time, exactly
/// as `SemanticGenerationController` does. Tests pass a fake that returns fake
/// thumbnail/client/credential-store seams and a shared gate.
class IImageAnalysisEnvironment {
 public:
  virtual ~IImageAnalysisEnvironment()                                                    = default;
  virtual auto ThumbnailProvider() -> std::shared_ptr<IImageAnalysisThumbnailProvider>    = 0;
  virtual auto AnalysisClient() -> std::shared_ptr<IImageAnalysisClient>                  = 0;
  virtual auto CredentialStore() -> std::shared_ptr<IAiCredentialStore>                   = 0;
  virtual auto Gate() -> std::shared_ptr<ImageAnalysisInFlightGate>                       = 0;
  /// Optional per-image context for remote analysis. Production reads non-secret
  /// EXIF/camera metadata; tests may return any deterministic string. The
  /// controller only requests and forwards it for gear-sensitive rating/analyze
  /// runs.
  virtual auto CameraContextForItem(const alcedo::ImageAnalysisItem& item) -> std::string = 0;
  virtual auto AcquireSidecarLease() -> std::shared_ptr<void>                             = 0;
  /// Start the AI sidecar on demand with `require_model_info=false` (remote image
  /// analysis uses the HTTP-provider path; no CLIP model is needed). Returns true
  /// if the sidecar is ready. Does NOT check `model_info` (it is unpopulated when
  /// `require_model_info=false`).
  virtual auto EnsureSidecarReady(bool provider_configs_dirty, std::string* error) -> bool = 0;
  /// Interactive variant of EnsureSidecarReady: pumps the Qt event loop during
  /// the sidecar boot so the UI stays responsive. MUST be called on the UI
  /// thread. The default falls back to the synchronous EnsureSidecarReady so
  /// unit-test fakes are unaffected (their boot is instant). Production
  /// overrides this to route through AiSidecarRuntimeService::StartAndWaitInteractive.
  virtual auto EnsureSidecarReadyInteractive(bool provider_configs_dirty, std::string* error)
      -> bool {
    return EnsureSidecarReady(provider_configs_dirty, error);
  }
  /// Request that an in-progress interactive boot abort at the next poll
  /// checkpoint. Default is a no-op (tests don't exercise boot cancel);
  /// production forwards to AiSidecarRuntimeService::RequestCancelStart.
  virtual void RequestSidecarStartCancel() {}
};

/// Drives remote image analysis (caption/tags via `image_understanding.describe`,
/// rating via `image_rating.score`) from the album workflow.
///
/// Mirrors the cleanly-factored QObject sub-controller pattern
/// (`SemanticGenerationController`, `ModelDownloadController`): own hpp/cpp under
/// `album_backend/`, surfaced to QML as `appModules.imageAnalysis`. It is
/// constructable with an `IImageAnalysisEnvironment` +
/// `AiProviderProfileController*` so it has no host dependency and is
/// unit-testable with fakes.
///
/// The controller owns NO database writes (persistence + search refresh is Phase
/// 6e). A cancelled or failed remote call therefore cannot upsert an active
/// understanding/rating row — the controller only surfaces results in QML state,
/// and failed/canceled items are never counted as `analyzed`. The sidecar is
/// started on demand with `require_model_info=false` so ordinary album
/// browsing/search requires neither a running sidecar nor an API key. Remote calls
/// are serialized through one shared `ImageAnalysisInFlightGate` (Phase 6d mandate)
/// passed via the environment, so jobs serialize app-wide, not per service instance.
class ImageAnalysisController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool running READ Running NOTIFY StateChanged)
  Q_PROPERTY(int total READ Total NOTIFY StateChanged)
  Q_PROPERTY(int analyzed READ Analyzed NOTIFY StateChanged)
  Q_PROPERTY(int failed READ Failed NOTIFY StateChanged)
  Q_PROPERTY(int canceled READ Canceled NOTIFY StateChanged)
  Q_PROPERTY(QString statusText READ StatusText NOTIFY StateChanged)
  Q_PROPERTY(QString lastError READ LastError NOTIFY StateChanged)
  Q_PROPERTY(bool canRetry READ CanRetry NOTIFY StateChanged)
  Q_PROPERTY(bool providerConfigured READ ProviderConfigured NOTIFY StateChanged)
  Q_PROPERTY(bool credentialAvailable READ CredentialAvailable NOTIFY StateChanged)
  Q_PROPERTY(QVariantList lastResults READ LastResults NOTIFY StateChanged)
  Q_PROPERTY(QVariantMap lastUsage READ LastUsage NOTIFY StateChanged)
  Q_PROPERTY(QVariantList discoveredModels READ DiscoveredModels NOTIFY StateChanged)
  Q_PROPERTY(QString connectionStatus READ ConnectionStatus NOTIFY StateChanged)
  // Rating strictness persona for the ScoreImage task: "lite" | "normal" |
  // "high" | "xhigh" | "max". Persisted across sessions via QSettings
  // (ai/analysis/ratingSeverity), default "normal". Drives the rating system
  // prompt the sidecar builds.
  Q_PROPERTY(QString ratingSeverity READ RatingSeverity NOTIFY StateChanged)

 public:
  ImageAnalysisController(std::shared_ptr<IImageAnalysisEnvironment> env,
                          AiProviderProfileController*               profiles,
                          std::shared_ptr<IImageAnalysisSink>        sink,
                          BackgroundTaskController* registry = nullptr, QObject* parent = nullptr);

  bool             Running() const { return running_; }
  int              Total() const { return total_; }
  int              Analyzed() const { return analyzed_; }
  int              Failed() const { return failed_; }
  int              Canceled() const { return canceled_; }
  QString          StatusText() const { return status_text_.Render(); }
  QString          LastError() const { return last_error_; }
  bool             CanRetry() const { return can_retry_; }
  bool             ProviderConfigured() const { return provider_configured_; }
  bool             CredentialAvailable() const { return credential_available_; }
  QVariantList     LastResults() const { return last_results_; }
  QVariantMap      LastUsage() const { return last_usage_; }
  QVariantList     DiscoveredModels() const { return discovered_models_; }
  QString          ConnectionStatus() const { return connection_status_; }
  QString          RatingSeverity() const { return rating_severity_; }

  // Album selection is a QVariantList of {elementId, imageId} maps (the same
  // convention as ImportExportHandler::CollectExportTargets). Empty selection is
  // a no-op with a clear error — image analysis is a paid remote call, so it must
  // never silently fall back to "whole view".
  Q_INVOKABLE void StartDescribeForTargets(const QVariantList& targetEntries);
  Q_INVOKABLE void StartScoreForTargets(const QVariantList& targetEntries,
                                        bool                includeRatingReasons);
  Q_INVOKABLE void StartAnalyzeForTargets(const QVariantList& targetEntries,
                                          bool                includeRatingReasons);
  Q_INVOKABLE void CancelAnalysis();
  Q_INVOKABLE void RetryLast();
  // Dry-run model discovery against the selected profile (reuses the Phase 6c
  // ValidateConnection path). Surfaces ok/error in lastError and populates
  // `discoveredModels` with the live-listed candidates. The sidecar commits
  // discovered models during ListModels, so a candidate is immediately
  // selectable as an explicit model_id.
  Q_INVOKABLE void ValidateConnection();
  Q_INVOKABLE void ValidateConnectionForProfile(const QString& profileId);
  Q_INVOKABLE void RefreshCredentialState();
  // Set the rating strictness persona ("lite" | "normal" | "high" | "xhigh" |
  // "max"). Unknown values clamp to "normal". Persisted to QSettings
  // (ai/analysis/ratingSeverity) so the choice survives across sessions.
  Q_INVOKABLE bool SetRatingSeverity(const QString& value);

 signals:
  void StateChanged();

 private:
  void StartForTargets(const QVariantList& targetEntries, alcedo::ImageAnalysisTask task,
                       bool includeRatingReasons);
  auto CollectItems(const QVariantList& targetEntries) -> std::vector<alcedo::ImageAnalysisItem>;
  void RefreshConfiguredState();
  void UpdateProgress(const alcedo::ImageAnalysisProgress& progress);
  void Finish(std::vector<alcedo::ImageAnalysisItemResult> results);
  void SetError(const QString& error);
  void ResetCounters();
  // Build the `affectedTargets` list ({elementId,imageId} maps) for the task
  // snapshot from the in-flight `last_items_` set.
  auto BuildAffectedTargets() const -> QVariantList;
  // Register this run as a background task (Phase 1 mirroring) and return the
  // assigned task id. No-op (returns empty) when no registry was injected.
  auto RegisterBackgroundTask() -> QString;

  std::shared_ptr<IImageAnalysisEnvironment> env_;
  AiProviderProfileController*               profiles_;
  std::shared_ptr<IImageAnalysisSink>        sink_;
  std::shared_ptr<alcedo::ImageAnalysisJob>  job_;
  std::shared_ptr<void>                      sidecar_lease_;
  // Phase 1 background-task mirroring. `registry_` is optional (tests inject
  // none); when present, the controller registers/updates/finishes a task the
  // QML task bar mirrors. Cancel from the bar routes back to CancelAnalysis().
  BackgroundTaskController*                  registry_ = nullptr;
  QString                                    background_task_id_;
  std::vector<alcedo::ImageAnalysisItem>     last_items_;
  alcedo::ImageAnalysisTask                  last_task_ = alcedo::ImageAnalysisTask::kDescribe;
  bool                                       last_include_rating_reasons_ = true;
  // True when the user pressed Cancel during the interactive sidecar boot
  // (before job_ exists). The boot-failure path reads this to finish the task
  // as Canceled instead of Failed.
  bool                                       start_canceled_ = false;

  i18n::LocalizedText                        status_text_{};
  QString                                    last_error_;
  QString                                    connection_status_;
  QString                                    rating_severity_ = QStringLiteral("normal");
  QVariantList                               last_results_;
  QVariantList                               discovered_models_;
  QVariantMap                                last_usage_;
  bool                                       running_              = false;
  bool                                       can_retry_            = false;
  bool                                       provider_configured_  = false;
  bool                                       credential_available_ = false;
  int                                        total_                = 0;
  int                                        analyzed_             = 0;
  int                                        failed_               = 0;
  int                                        canceled_             = 0;
};

/// Production environment: resolves runtime seams from ProjectModule / semantic
/// generation / AI profiles / gate at call time (no host dependency).
std::shared_ptr<IImageAnalysisEnvironment> MakeAlbumImageAnalysisEnvironment(
    ProjectModule* project, SemanticGenerationController* semantic,
    alcedo::AiProviderProfileController* profiles,
    std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate);

/// Production Phase 7a sink. Delegates to AiStore, ImageController,
/// and StatsEngine. Queues writes behind ProjectDbWriteBarrier when held.
std::shared_ptr<IImageAnalysisSink> MakeAlbumImageAnalysisSink(
    ProjectModule* project, ImageController* images, StatsEngine* stats,
    ProjectDbWriteBarrier* barrier);

}  // namespace alcedo::ui
