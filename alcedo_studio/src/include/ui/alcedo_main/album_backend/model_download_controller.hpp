//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <chrono>
#include <optional>

#include "app/model_download_service.hpp"
#include "app/semantic_runtime_service.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class AlbumBackend;

namespace detail {
// Reads the on-disk resolved manifest for a profile back into a
// SemanticResolvedModelManifest, validating it against the catalog. Kept as a
// free function in `detail` so the activation path and the unit test can reach
// it without depending on the controller instance.
auto LoadLocalResolvedModelManifestForActivation(const QString& profileId,
                                                 const QString& baseDirectory, QString* error)
    -> std::optional<SemanticResolvedModelManifest>;
}  // namespace detail

// Owns all model-download / install / settings state and the communication with
// ModelDownloadService. This is the "is the model downloaded, where does it
// live, how is the download going" concern — deliberately kept separate from
// SemanticGenerationController, which only consumes the install state to
// activate a model and run generation.
class ModelDownloadController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList modelProfileOptions READ ModelProfileOptions CONSTANT)
  Q_PROPERTY(QString selectedModelProfileId READ SelectedModelProfileId NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadDirectory READ ModelDownloadDirectory NOTIFY StateChanged)
  Q_PROPERTY(QString modelEndpointPreset READ ModelEndpointPreset NOTIFY StateChanged)
  Q_PROPERTY(QString customModelEndpoint READ CustomModelEndpoint NOTIFY StateChanged)
  Q_PROPERTY(QString effectiveModelEndpoint READ EffectiveModelEndpoint NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadStatusText READ ModelDownloadStatusText NOTIFY StateChanged)
  Q_PROPERTY(bool modelDownloadRunning READ ModelDownloadRunning NOTIFY StateChanged)
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

 public:
  explicit ModelDownloadController(AlbumBackend& backend, QObject* parent = nullptr);

  QVariantList ModelProfileOptions() const;
  QString      SelectedModelProfileId() const;
  QString      ModelDownloadDirectory() const;
  QString      ModelEndpointPreset() const;
  QString      CustomModelEndpoint() const;
  QString      EffectiveModelEndpoint() const;
  QString      ModelDownloadStatusText() const { return model_download_status_text_.Render(); }
  bool         ModelDownloadRunning() const { return model_download_running_; }
  int          ModelDownloadProgress() const { return model_download_progress_; }
  QString      ModelDownloadPhase() const { return model_download_phase_; }
  QString      ModelDownloadCurrentFile() const { return model_download_current_file_; }
  quint64      ModelDownloadBytesDone() const { return model_download_bytes_done_; }
  quint64      ModelDownloadBytesTotal() const { return model_download_bytes_total_; }
  QString      ModelDownloadBytesLabel() const { return model_download_bytes_label_; }
  QString      ModelDownloadSpeedLabel() const { return model_download_speed_label_; }
  QString      ModelDownloadEtaLabel() const { return model_download_eta_label_; }
  int          ModelDownloadFilesDone() const { return model_download_files_done_; }
  int          ModelDownloadFilesTotal() const { return model_download_files_total_; }
  QString      SelectedModelSizeLabel() const;
  bool         SelectedModelInstalled() const { return selected_model_installed_; }

  Q_INVOKABLE void SetSelectedModelProfileId(const QString& profileId);
  Q_INVOKABLE void SetModelDownloadDirectory(const QString& directory);
  Q_INVOKABLE void SetModelEndpointPreset(const QString& preset);
  Q_INVOKABLE void SetCustomModelEndpoint(const QString& endpoint);
  Q_INVOKABLE void ResetModelDownloadDirectory();
  Q_INVOKABLE void RefreshSelectedModelStatus();
  Q_INVOKABLE void StartSelectedModelDownload();
  Q_INVOKABLE void CancelSelectedModelDownload();
  Q_INVOKABLE void DeleteSelectedModel();

  // ── Consumed by SemanticGenerationController (activation / runtime options) ──
  // Loads the resolved manifest for the currently selected profile. Returns
  // nullopt and fills *error when the profile is not installed or the manifest
  // is invalid.
  [[nodiscard]] auto LoadSelectedResolvedManifest(QString* error) const
      -> std::optional<SemanticResolvedModelManifest>;
  // Absolute on-disk root for a profile under the configured download
  // directory: <modelDownloadDirectory>/<profileId>.
  [[nodiscard]] auto ModelRootForProfile(const QString& profileId) const
      -> std::filesystem::path;
  // Surface activation progress/errors in the shared model status card.
  // SemanticGenerationController (which owns model activation) calls this so the
  // card — bound to this controller's modelDownloadStatusText — reflects
  // activation state too. Emits StateChanged.
  void SetStatusText(const i18n::LocalizedText& text);

 signals:
  // Coarse notify for all Q_PROPERTYs above (mirrors the existing controller
  // convention of one shared NOTIFY signal).
  void StateChanged();
  // Emitted only when the selected profile or its on-disk install state actually
  // changes (selection change, download finish, delete, refresh) — not on every
  // 250 ms progress tick. SemanticGenerationController listens to this to
  // recompute its `selectedModelActive` badge without churning on progress.
  void SelectedModelInstallChanged();

 private:
  // Recomputes selected_model_installed_ from disk + catalog. Does not emit;
  // callers emit StateChanged (and SelectedModelInstallChanged when it changed).
  void RecomputeSelectedModelState();
  // Updates the EMA download speed + ETA labels from a progress tick. Only
  // advances the average while phase == "downloading"; clears the labels (but
  // keeps the EMA) otherwise so inter-asset gaps don't flicker.
  void UpdateDownloadSpeed(const alcedo::ModelDownloadProgress& progress);

  AlbumBackend&         backend_;
  i18n::LocalizedText   model_download_status_text_{};
  bool                  model_download_running_      = false;
  int                   model_download_progress_    = 0;
  QString               model_download_phase_{};
  QString               model_download_current_file_{};
  quint64               model_download_bytes_done_  = 0;
  quint64               model_download_bytes_total_ = 0;
  QString               model_download_bytes_label_{};
  QString               model_download_speed_label_{};
  QString               model_download_eta_label_{};
  int                   model_download_files_done_  = 0;
  int                   model_download_files_total_ = 0;
  // EMA download-speed tracking. Sampled in the ProgressChanged handler only
  // while phase == "downloading"; the baseline is re-primed whenever download
  // resumes so inter-asset validation gaps don't produce spikes.
  std::chrono::steady_clock::time_point download_sample_time_{};
  quint64                               download_sample_bytes_ = 0;
  double                                download_speed_ema_    = 0.0;
  bool                                  download_speed_primed_ = false;
  bool                                  selected_model_installed_ = false;
};

}  // namespace alcedo::ui
