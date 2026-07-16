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
#include "app/semantic_generation_service.hpp"
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
// ModelDownloadController; this controller consumes the install state only to
// activate a model and to derive the `selectedModelActive` badge.
class SemanticGenerationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool promptVisible READ PromptVisible NOTIFY StateChanged)
  Q_PROPERTY(bool activatePromptVisible READ ActivatePromptVisible NOTIFY StateChanged)
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
  Q_PROPERTY(bool selectedModelActive READ SelectedModelActive NOTIFY StateChanged)

 public:
  SemanticGenerationController(ProjectModule* project, LibraryModule* library,
                               ModelDownloadController* model_download,
                               BackgroundTaskController* background_tasks, IUiStatusSink* status,
                               alcedo::AiProviderProfileController* ai_profiles,
                               QObject* parent = nullptr);

  void BindCollaborators(NikonHeRecoveryController* nikon);

  bool               PromptVisible() const;
  bool               ActivatePromptVisible() const;
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
  bool               SelectedModelActive() const { return selected_model_active_; }

  Q_INVOKABLE void   StartPendingGeneration(bool forceRegenerate = false);
  Q_INVOKABLE void   SkipPendingGeneration(bool rememberChoice = false);
  Q_INVOKABLE void   DismissActivatePrompt();
  Q_INVOKABLE void   SetImportPreference(const QString& preference);
  Q_INVOKABLE void   ActivateSelectedModel();
  Q_INVOKABLE void   CancelGeneration();
  Q_INVOKABLE void   RefreshAlbumSummary();
  Q_INVOKABLE void   StartAlbumGeneration(bool forceRegenerate = false);

  void               QueuePrompt(std::vector<SemanticGenerationItem> items);
  void               ResumeQueuedWorkflow();

  [[nodiscard]] auto ActiveModelKey() const -> std::string;
  [[nodiscard]] auto RuntimeOptionsForCurrentSidecarSnapshot(bool requireModelInfo) const
      -> AiSidecarRuntimeOptions;
  [[nodiscard]] auto LabelDisplayText(sl_element_id_t elementId) const -> QString;
  // Full post-open / post-purge refresh: fresh on-disk install state, a
  // recomputed selectedModelActive badge, album counts, and an unconditional
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
  auto RegisterActivationTask() -> QString;
  // Recomputes selected_model_active_ from the download controller's install
  // state and the project's active-model record. Does not emit StateChanged;
  // callers emit.
  void RecomputeSelectedModelActive();
  // Recomputes the selectedModelActive badge, then - if the selected model is
  // installed, not already active, and already has label prototypes cached ("warm") -
  // flips the project's active model to it so routing and label caching switch
  // instantly without pressing Activate. No-op for cold models (the user must
  // Activate to generate the cache). Idempotent and guarded; safe to call from the
  // selection signal and from RefreshSemanticState (project open).
  void TryAutoActivateSelectedModel();
  [[nodiscard]] auto RuntimeOptionsForProfile(const QString& profileId, bool profileRoot) const
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
  i18n::LocalizedText                    status_text_{};
  i18n::LocalizedText                    album_summary_text_{};
  std::string                            model_key_{};
  bool                                   model_activation_running_ = false;
  bool                                   selected_model_active_    = false;
  bool                                   prompt_pending_           = false;
  bool                                   activate_prompt_pending_  = false;
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

}  // namespace alcedo::ui
