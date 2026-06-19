//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/model_download_service.hpp"
#include "app/semantic_generation_service.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class AlbumBackend;
class SemanticRuntimeSessionGuard;

namespace detail {
auto LoadLocalResolvedModelManifestForActivation(const QString& profileId,
                                                 const QString& baseDirectory, QString* error)
    -> std::optional<SemanticResolvedModelManifest>;
}  // namespace detail

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
  Q_PROPERTY(QVariantList modelProfileOptions READ ModelProfileOptions CONSTANT)
  Q_PROPERTY(QString selectedModelProfileId READ SelectedModelProfileId NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelProfileId READ ActiveModelProfileId NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelDisplayName READ ActiveModelDisplayName NOTIFY StateChanged)
  Q_PROPERTY(QString activeModelKey READ ActiveModelKeyQString NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadDirectory READ ModelDownloadDirectory NOTIFY StateChanged)
  Q_PROPERTY(QString modelEndpointPreset READ ModelEndpointPreset NOTIFY StateChanged)
  Q_PROPERTY(QString customModelEndpoint READ CustomModelEndpoint NOTIFY StateChanged)
  Q_PROPERTY(QString effectiveModelEndpoint READ EffectiveModelEndpoint NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadStatusText READ ModelDownloadStatusText NOTIFY StateChanged)
  Q_PROPERTY(bool modelDownloadRunning READ ModelDownloadRunning NOTIFY StateChanged)
  Q_PROPERTY(bool modelActivationRunning READ ModelActivationRunning NOTIFY StateChanged)
  Q_PROPERTY(int modelDownloadProgress READ ModelDownloadProgress NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadPhase READ ModelDownloadPhase NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadCurrentFile READ ModelDownloadCurrentFile NOTIFY StateChanged)
  Q_PROPERTY(quint64 modelDownloadBytesDone READ ModelDownloadBytesDone NOTIFY StateChanged)
  Q_PROPERTY(quint64 modelDownloadBytesTotal READ ModelDownloadBytesTotal NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadBytesLabel READ ModelDownloadBytesLabel NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadSpeedLabel READ ModelDownloadSpeedLabel NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadEtaLabel READ ModelDownloadEtaLabel NOTIFY StateChanged)
  Q_PROPERTY(int modelDownloadFilesDone READ ModelDownloadFilesDone NOTIFY StateChanged)
  Q_PROPERTY(int modelDownloadFilesTotal READ ModelDownloadFilesTotal NOTIFY StateChanged)
  Q_PROPERTY(QString selectedModelSizeLabel READ SelectedModelSizeLabel NOTIFY StateChanged)
  Q_PROPERTY(bool selectedModelInstalled READ SelectedModelInstalled NOTIFY StateChanged)
  Q_PROPERTY(bool selectedModelActive READ SelectedModelActive NOTIFY StateChanged)

 public:
  explicit SemanticGenerationController(AlbumBackend& backend, QObject* parent = nullptr);

  bool             PromptVisible() const;
  bool             Running() const { return running_; }
  int              PendingCount() const { return static_cast<int>(pending_items_.size()); }
  int              Total() const { return total_; }
  int              Embedded() const { return embedded_; }
  int              Skipped() const { return skipped_; }
  int              Failed() const { return failed_; }
  int              Canceled() const { return canceled_; }
  QString          StatusText() const { return status_text_.Render(); }
  int              AlbumTotalCount() const { return album_total_count_; }
  int              AlbumLabeledCount() const { return album_labeled_count_; }
  int              AlbumUnlabeledCount() const { return album_unlabeled_count_; }
  QString          AlbumSummaryText() const { return album_summary_text_.Render(); }
  QString          ImportPreference() const;
  QVariantList     ModelProfileOptions() const;
  QString          SelectedModelProfileId() const;
  QString          ActiveModelProfileId() const;
  QString          ActiveModelDisplayName() const;
  QString          ActiveModelKeyQString() const;
  QString          ModelDownloadDirectory() const;
  QString          ModelEndpointPreset() const;
  QString          CustomModelEndpoint() const;
  QString          EffectiveModelEndpoint() const;
  QString          ModelDownloadStatusText() const { return model_download_status_text_.Render(); }
  bool             ModelDownloadRunning() const { return model_download_running_; }
  bool             ModelActivationRunning() const { return model_activation_running_; }
  int              ModelDownloadProgress() const { return model_download_progress_; }
  QString          ModelDownloadPhase() const { return model_download_phase_; }
  QString          ModelDownloadCurrentFile() const { return model_download_current_file_; }
  quint64          ModelDownloadBytesDone() const { return model_download_bytes_done_; }
  quint64          ModelDownloadBytesTotal() const { return model_download_bytes_total_; }
  QString          ModelDownloadBytesLabel() const { return model_download_bytes_label_; }
  QString          ModelDownloadSpeedLabel() const { return model_download_speed_label_; }
  QString          ModelDownloadEtaLabel() const { return model_download_eta_label_; }
  int              ModelDownloadFilesDone() const { return model_download_files_done_; }
  int              ModelDownloadFilesTotal() const { return model_download_files_total_; }
  QString          SelectedModelSizeLabel() const;
  bool             SelectedModelInstalled() const { return selected_model_installed_; }
  bool             SelectedModelActive() const { return selected_model_active_; }

  Q_INVOKABLE void StartPendingGeneration(bool forceRegenerate = false);
  Q_INVOKABLE void SkipPendingGeneration(bool rememberChoice = false);
  Q_INVOKABLE void SetImportPreference(const QString& preference);
  Q_INVOKABLE void SetSelectedModelProfileId(const QString& profileId);
  Q_INVOKABLE void SetModelDownloadDirectory(const QString& directory);
  Q_INVOKABLE void SetModelEndpointPreset(const QString& preset);
  Q_INVOKABLE void SetCustomModelEndpoint(const QString& endpoint);
  Q_INVOKABLE void ResetModelDownloadDirectory();
  Q_INVOKABLE void RefreshSelectedModelStatus();
  Q_INVOKABLE void StartSelectedModelDownload();
  Q_INVOKABLE void CancelSelectedModelDownload();
  Q_INVOKABLE void DeleteSelectedModel();
  Q_INVOKABLE void ActivateSelectedModel();
  Q_INVOKABLE void CancelGeneration();
  Q_INVOKABLE void RefreshAlbumSummary();
  Q_INVOKABLE void StartAlbumGeneration(bool forceRegenerate = false);

  void             QueuePrompt(std::vector<SemanticGenerationItem> items);
  void             ResumeQueuedWorkflow();

  [[nodiscard]] auto ActiveModelKey() const -> std::string;
  [[nodiscard]] auto LabelDisplayText(sl_element_id_t elementId) const -> QString;

 signals:
  void StateChanged();

 private:
  [[nodiscard]] auto StoredModelKey() const -> std::string;
  void StartGenerationForItems(std::vector<SemanticGenerationItem> items, bool forceRegenerate);
  void ContinueGenerationForItems(bool forceRegenerate);
  void UpdateProgress(const SemanticGenerationProgress& progress);
  void Finish(std::vector<SemanticGenerationItemResult> results);
  void ClearPrompt();
  void ResetCounters();
  // Recomputes selected_model_installed_ / selected_model_active_ from disk and
  // the active-model record. Does not emit StateChanged; callers emit.
  void RecomputeSelectedModelState();
  // Updates the EMA download speed + ETA labels from a progress tick. Only
  // advances the average while phase == "downloading"; clears the labels (but
  // keeps the EMA) otherwise so inter-asset gaps don't flicker.
  void UpdateDownloadSpeed(const alcedo::ModelDownloadProgress& progress);
  [[nodiscard]] auto RuntimeOptionsForProfile(const QString& profileId, bool profileRoot) const
      -> SemanticRuntimeOptions;

  AlbumBackend&                                backend_;
  std::vector<SemanticGenerationItem>          pending_items_{};
  std::shared_ptr<SemanticRuntimeSessionGuard> runtime_session_{};
  std::shared_ptr<SemanticGenerationJob>       job_{};
  i18n::LocalizedText                          status_text_{};
  i18n::LocalizedText                          album_summary_text_{};
  i18n::LocalizedText                          model_download_status_text_{};
  std::string                                  model_key_{};
  bool                                         model_download_running_   = false;
  bool                                         model_activation_running_ = false;
  int                                          model_download_progress_  = 0;
  QString                                      model_download_phase_{};
  QString                                      model_download_current_file_{};
  quint64                                      model_download_bytes_done_   = 0;
  quint64                                      model_download_bytes_total_   = 0;
  QString                                      model_download_bytes_label_{};
  QString                                      model_download_speed_label_{};
  QString                                      model_download_eta_label_{};
  int                                          model_download_files_done_   = 0;
  int                                          model_download_files_total_   = 0;
  // EMA download-speed tracking. Sampled in the ProgressChanged handler only
  // while phase == "downloading"; the baseline is re-primed whenever download
  // resumes so inter-asset validation gaps don't produce spikes.
  std::chrono::steady_clock::time_point        download_sample_time_{};
  quint64                                      download_sample_bytes_ = 0;
  double                                       download_speed_ema_    = 0.0;
  bool                                         download_speed_primed_ = false;
  bool                                         selected_model_installed_ = false;
  bool                                         selected_model_active_    = false;
  bool                                         prompt_pending_           = false;
  bool                                         running_                  = false;
  int                                          total_                    = 0;
  int                                          embedded_                 = 0;
  int                                          skipped_                  = 0;
  int                                          failed_                   = 0;
  int                                          canceled_                 = 0;
  int                                          album_total_count_        = 0;
  int                                          album_labeled_count_      = 0;
  int                                          album_unlabeled_count_    = 0;
};

}  // namespace alcedo::ui
