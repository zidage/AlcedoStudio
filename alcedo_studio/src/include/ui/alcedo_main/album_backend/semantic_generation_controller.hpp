//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>
#include <string>
#include <vector>

#include "app/ai_provider_profile.hpp"
#include "app/model_asset_catalog.hpp"
#include "app/semantic_generation_service.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class BackgroundTaskController;
class LibraryModule;
class ModelDownloadController;
class NikonHeRecoveryController;
class ProjectModule;
class IUiStatusSink;

// Drives semantic (AI content) label generation and model activation. Owns the
// generation pipeline state (progress, prompt, album summary) and the
// active-model surface. Model download / install / settings live in
// ModelDownloadController; this controller reads the install state to activate an
// installed model for the open project (activation per project, AI Models plan A3).
class SemanticGenerationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool promptVisible READ PromptVisible NOTIFY StateChanged)
  Q_PROPERTY(bool running READ Running NOTIFY StateChanged)
  Q_PROPERTY(int pendingCount READ PendingCount NOTIFY StateChanged)
  Q_PROPERTY(int total READ Total NOTIFY StateChanged)
  Q_PROPERTY(int embedded READ Embedded NOTIFY StateChanged)
  Q_PROPERTY(int skipped READ Skipped NOTIFY StateChanged)
  Q_PROPERTY(int failed READ Failed NOTIFY StateChanged)
  Q_PROPERTY(int canceled READ Canceled NOTIFY StateChanged)
  Q_PROPERTY(QString statusText READ StatusText NOTIFY StateChanged)
  Q_PROPERTY(int albumTotalCount READ AlbumTotalCount NOTIFY StateChanged)
  Q_PROPERTY(int albumLabeledCount READ AlbumLabeledCount NOTIFY StateChanged)
  Q_PROPERTY(int albumUnlabeledCount READ AlbumUnlabeledCount NOTIFY StateChanged)
  Q_PROPERTY(QString albumSummaryText READ AlbumSummaryText NOTIFY StateChanged)
  Q_PROPERTY(QString importPreference READ ImportPreference NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelProfileId READ ActiveModelProfileId NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelDisplayName READ ActiveModelDisplayName NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelKey READ ActiveModelKeyQString NOTIFY StateChanged)
  Q_PROPERTY(bool modelActivationRunning READ ModelActivationRunning NOTIFY StateChanged)
  /// Profile that activateModel() is activating; empty when no activation runs.
  Q_PROPERTY(QString activatingModelProfileId READ ActivatingModelProfileId NOTIFY StateChanged)
  /// "Keep AI model loaded" in seconds: 0, 60, 300, 900, or -1 (while the project is open).
  Q_PROPERTY(int sidecarIdleStopSeconds READ SidecarIdleStopSeconds WRITE SetSidecarIdleStopSeconds
                 NOTIFY StateChanged)

 public:
  SemanticGenerationController(ProjectModule* project, LibraryModule* library,
                               ModelDownloadController* model_download,
                               BackgroundTaskController* background_tasks, IUiStatusSink* status,
                               alcedo::AiProviderProfileController* ai_profiles,
                               QObject* parent = nullptr);

  void BindCollaborators(NikonHeRecoveryController* nikon);

  bool               PromptVisible() const;
  bool               Running() const { return running_; }
  int                PendingCount() const { return static_cast<int>(pending_items_.size()); }
  int                Total() const { return total_; }
  int                Embedded() const { return embedded_; }
  int                Skipped() const { return skipped_; }
  int                Failed() const { return failed_; }
  int                Canceled() const { return canceled_; }
  QString            StatusText() const { return status_text_.Render(); }
  int                AlbumTotalCount() const { return album_total_count_; }
  int                AlbumLabeledCount() const { return album_labeled_count_; }
  int                AlbumUnlabeledCount() const { return album_unlabeled_count_; }
  QString            AlbumSummaryText() const { return album_summary_text_.Render(); }
  QString            ImportPreference() const;
  QString            ActiveModelProfileId() const;
  QString            ActiveModelDisplayName() const;
  QString            ActiveModelKeyQString() const;
  bool               ModelActivationRunning() const { return model_activation_running_; }
  QString            ActivatingModelProfileId() const { return activating_profile_id_; }
  int                SidecarIdleStopSeconds() const;
  /// Writes the setting and applies it at once to the sidecar process of the open project
  /// (ProjectService::SetAiSidecarIdleStopSeconds). The text encoder idle time follows at the
  /// next sidecar start.
  void               SetSidecarIdleStopSeconds(int seconds);

  Q_INVOKABLE void   StartPendingGeneration(bool forceRegenerate = false);
  Q_INVOKABLE void   SkipPendingGeneration(bool rememberChoice = false);
  Q_INVOKABLE void   SetImportPreference(const QString& preference);
  /// Activates the installed model @p profileId for the open project: registers it, computes
  /// the label prototypes when they are not cached (sidecar), and sets the project's active
  /// model key. Returns false, with the reason in the error of the model's row in
  /// ModelDownloadController, without an open project, for a model that is not installed or
  /// already active, and while generation or another activation runs.
  bool               ActivateModel(const QString& profileId);
  Q_INVOKABLE bool   activateModel(const QString& profileId) { return ActivateModel(profileId); }
  Q_INVOKABLE void   CancelGeneration();
  Q_INVOKABLE void   RefreshAlbumSummary();
  Q_INVOKABLE void   StartAlbumGeneration(bool forceRegenerate = false);

  void               QueuePrompt(std::vector<SemanticGenerationItem> items);
  void               ResumeQueuedWorkflow();

  [[nodiscard]] auto ActiveModelKey() const -> std::string;
  [[nodiscard]] auto RuntimeOptionsForCurrentSidecarSnapshot(bool requireModelInfo) const
      -> AiSidecarRuntimeOptions;
  [[nodiscard]] auto LabelDisplayText(sl_element_id_t elementId) const -> QString;
  // Full post-open / post-purge refresh: fresh on-disk install state, album
  // counts, and an unconditional
  // StateChanged so live DB bindings (activeModelName/activeModelKey) refresh
  // even when the counts are unchanged. Called on project open and after a
  // save-time purge.
  void               RefreshSemanticState();

 signals:
  void StateChanged();

 private:
  [[nodiscard]] auto StoredModelKey() const -> std::string;
  // A project is "fresh" w.r.t. the semantic feature when no model has ever
  // been registered in its DB (no SemanticModel rows). That also implies no
  // active model and no embeddings, since both require a registered model.
  [[nodiscard]] bool IsFreshProject() const;
  void StartGenerationForItems(std::vector<SemanticGenerationItem> items, bool forceRegenerate);
  void ContinueGenerationForItems(bool forceRegenerate);
  void UpdateProgress(const SemanticGenerationProgress& progress);
  void Finish(std::vector<SemanticGenerationItemResult> results);
  void ClearPrompt();
  void ResetCounters();
  // Build the `affectedTargets` list ({elementId,imageId} maps) for the task
  // snapshot from the in-flight `pending_items_` set.
  auto BuildAffectedTargets() const -> QVariantList;
  // Register this run as a background task (Phase 1 mirroring) and return the
  // assigned task id. No-op (returns empty) when no registry is reachable.
  auto RegisterBackgroundTask() -> QString;
  // Phase 2: register the model-activation run as a background task (non-
  // cancelable — activation runs on a detached thread with no cancel path; Phase
  // 5 owns the shutdown wait) and return its id. No-op (returns empty) when no
  // registry is reachable. Publishes locks for sidecar snapshot settings so the
  // policy blocks model/provider/download changes during activation.
  auto RegisterActivationTask(const QString& profileId) -> QString;
  // Fails an activation request of @p profileId: the reason goes to the model's row.
  bool RejectActivation(const QString& profileId, const i18n::LocalizedText& reason);
  // After the files of @p profileId are removed: when it is the open project's active model,
  // the project has no active model afterwards (its rows stay until the save purge).
  void OnModelRemoved(const QString& profileId);
  // Sidecar options for an installed catalog profile.
  [[nodiscard]] auto RuntimeOptionsForProfile(const ModelProfileSpec& profile) const
      -> AiSidecarRuntimeOptions;

  ProjectModule*                            project_          = nullptr;
  LibraryModule*                            library_          = nullptr;
  ModelDownloadController*                  model_download_   = nullptr;
  BackgroundTaskController*                 background_tasks_ = nullptr;
  IUiStatusSink*                            status_           = nullptr;
  alcedo::AiProviderProfileController*      ai_profiles_      = nullptr;
  NikonHeRecoveryController*                nikon_            = nullptr;
  std::vector<SemanticGenerationItem>    pending_items_{};
  std::shared_ptr<SemanticGenerationJob> job_{};
  std::shared_ptr<void>                  sidecar_lease_{};
  // Phase 1 background-task mirroring id; empty when no task is registered.
  QString                                background_task_id_;
  // Phase 2: the model-activation background-task id (separate from the
  // generation task id so the two task lifecycles never collide). Empty when no
  // activation task is registered.
  QString                                model_activation_task_id_;
  QString                                activating_profile_id_;
  i18n::LocalizedText                    status_text_{};
  i18n::LocalizedText                    album_summary_text_{};
  std::string                            model_key_{};
  bool                                   model_activation_running_ = false;
  bool                                   prompt_pending_           = false;
  bool                                   running_                  = false;
  // True when the user pressed Cancel during the interactive sidecar boot
  // (before job_ exists). The boot-failure path reads this to finish the
  // background task as Canceled instead of Failed.
  bool                                   start_canceled_           = false;
  int                                    total_                    = 0;
  int                                    embedded_                 = 0;
  int                                    skipped_                  = 0;
  int                                    failed_                   = 0;
  int                                    canceled_                 = 0;
  int                                    album_total_count_        = 0;
  int                                    album_labeled_count_      = 0;
  int                                    album_unlabeled_count_    = 0;
};

/// Semantic label display language of the current UI language setting.
auto CurrentUiSemanticLabelLanguage() -> SemanticLabelLanguage;

/// Display text of the semantic labels stored for @p file_id under @p model_key: the stored top
/// labels joined by ", ", or empty when there is no label. Reads only @p storage, so the library
/// query worker calls it for the rows it reads.
auto ReadSemanticLabelDisplayText(Storage& storage, sl_element_id_t file_id,
                                  const std::string& model_key, SemanticLabelLanguage language)
    -> QString;

}  // namespace alcedo::ui
