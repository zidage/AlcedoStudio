//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <chrono>
#include <filesystem>
#include <optional>

#include "app/model_asset_catalog.hpp"
#include "app/model_download_service.hpp"
#include "sidecar_client/dto/model_manager.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class BackgroundTaskController;

namespace detail {
// Reads the on-disk resolved manifest of a profile of SemanticModelProfiles() back into a
// SemanticResolvedModelManifest, validating it against the catalog. A free function so the
// unit test can reach it without a controller.
auto LoadLocalResolvedModelManifestForActivation(const QString& profileId,
                                                 const QString& baseDirectory, QString* error)
    -> std::optional<SemanticResolvedModelManifest>;
}  // namespace detail

// Owns the install state of the CLIP model profiles: one row per profile of the platform with
// its own install, cancel, and remove (AI Models plan, AM3), the model directory, and the
// download source. SemanticGenerationController reads the install state to activate a model
// for the open project; activation is not part of this controller.
class ModelDownloadController final : public QObject {
  Q_OBJECT
  // One map per profile: profileId, displayName, modelId, language, inferenceBackend,
  // imageSize, sizeBytes, sizeLabel, licenseName, licenseUrl, installed, status ("notInstalled",
  // "installed", "downloading", "canceling"), busy, progress, phase, currentFile, bytesLabel,
  // speedLabel, etaLabel, filesDone, filesTotal, message, error.
  Q_PROPERTY(QVariantList models READ Models NOTIFY StateChanged)
  Q_PROPERTY(QString downloadingProfileId READ DownloadingProfileId NOTIFY StateChanged)
  Q_PROPERTY(bool modelDownloadRunning READ ModelDownloadRunning NOTIFY StateChanged)
  Q_PROPERTY(QString modelDownloadDirectory READ ModelDownloadDirectory NOTIFY StateChanged)
  Q_PROPERTY(QString modelEndpointPreset READ ModelEndpointPreset NOTIFY StateChanged)
  Q_PROPERTY(QString customModelEndpoint READ CustomModelEndpoint NOTIFY StateChanged)
  Q_PROPERTY(QString effectiveModelEndpoint READ EffectiveModelEndpoint NOTIFY StateChanged)

 public:
  ModelDownloadController(alcedo::ModelDownloadService& service,
                          BackgroundTaskController* background_tasks, QObject* parent = nullptr);

  QVariantList       Models() const;
  QString            DownloadingProfileId() const { return downloading_profile_id_; }
  bool               ModelDownloadRunning() const { return !downloading_profile_id_.isEmpty(); }
  QString      ModelDownloadDirectory() const;
  QString      ModelEndpointPreset() const;
  QString      CustomModelEndpoint() const;
  QString      EffectiveModelEndpoint() const;

  // Starts the download of @p profileId. Returns false with the reason in the row error when
  // the profile is unknown or another download runs (one download at a time, plan P9).
  bool               InstallModel(const QString& profileId);
  // Cancels the running download of @p profileId. Returns false when it does not run.
  bool               CancelInstall(const QString& profileId);
  // Deletes the files of @p profileId. Returns false with the reason in the row error while its
  // download runs or when the delete fails. Emits ModelRemoved on success.
  bool               RemoveModel(const QString& profileId);
  // Reads the install state of every profile from disk again.
  void               RefreshModels();

  Q_INVOKABLE bool   installModel(const QString& profileId) { return InstallModel(profileId); }
  Q_INVOKABLE bool   cancelInstall(const QString& profileId) { return CancelInstall(profileId); }
  Q_INVOKABLE bool   removeModel(const QString& profileId) { return RemoveModel(profileId); }
  Q_INVOKABLE void   refreshModels() { RefreshModels(); }
  Q_INVOKABLE void SetModelDownloadDirectory(const QString& directory);
  Q_INVOKABLE void SetModelEndpointPreset(const QString& preset);
  Q_INVOKABLE void SetCustomModelEndpoint(const QString& endpoint);
  Q_INVOKABLE void   ResetModelDownloadDirectory();

  // ── Consumed by SemanticGenerationController (activation / runtime options) ──
  // The catalog profile of @p profileId (profile ID or model ID); nullptr when unknown.
  [[nodiscard]] auto FindProfile(const QString& profileId) const -> const ModelProfileSpec*;
  // True when every file of the profile is on disk with its catalog size and the resolved
  // manifest matches the catalog (no file is hashed).
  [[nodiscard]] bool IsModelInstalled(const QString& profileId) const;
  // Loads the resolved manifest of @p profileId. Returns nullopt and fills *error when the
  // profile is unknown, not installed, or the manifest is invalid.
  [[nodiscard]] auto LoadResolvedManifest(const QString& profileId, QString* error) const
      -> std::optional<SemanticResolvedModelManifest>;
  // Absolute on-disk root for a profile under the configured download
  // directory: <modelDownloadDirectory>/<profileId>.
  [[nodiscard]] auto ModelRootForProfile(const QString& profileId) const
      -> std::filesystem::path;
  // Shows an activation message on the row of @p profileId (SemanticGenerationController owns
  // activation). @p is_error puts the text into the row error. Emits StateChanged.
  void SetModelMessage(const QString& profileId, const i18n::LocalizedText& text, bool is_error);
  // Returns true if semantic data for `profileId` should be kept: the profile is
  // installed on disk, OR its install state is indeterminate (the profile is no
  // longer in the catalog). Returns false only when the profile is known to the
  // catalog and confirmed missing/incomplete on disk — the sole case in which the
  // caller (the save-time purge) drops its DB rows.
  bool ShouldKeepSemanticModelData(const QString& profileId) const;

 signals:
  // Coarse notify for all Q_PROPERTYs above (one shared NOTIFY signal).
  void StateChanged();
  // Emitted after the files of @p profileId are deleted. SemanticGenerationController leaves the
  // open project with no active model when that profile was active.
  void ModelRemoved(const QString& profileId);

 private:
  // Per-profile message of the last operation (download, removal, activation).
  struct RowMessage {
    i18n::LocalizedText text{};
    bool                is_error = false;
  };

  void SetRowMessage(const QString& profileId, const i18n::LocalizedText& text, bool is_error);
  void ResetDownloadProgress();
  // Updates the EMA download speed + ETA labels from a progress tick. Only
  // advances the average while phase == "downloading"; clears the labels (but
  // keeps the EMA) otherwise so inter-asset gaps don't flicker.
  void UpdateDownloadSpeed(const alcedo::ModelDownloadProgress& progress);
  // Registers the download of @p profileId as a cancelable background task and returns its id.
  auto RegisterBackgroundTask(const QString& profileId) -> QString;

  alcedo::ModelDownloadService& service_;
  BackgroundTaskController*     background_tasks_ = nullptr;
  QString                               background_task_id_;
  QHash<QString, bool>                  installed_{};
  QHash<QString, RowMessage>            messages_{};
  // Profile of the running download; empty when none runs.
  QString                               downloading_profile_id_{};
  bool                                  download_canceling_      = false;
  int                                   model_download_progress_ = 0;
  QString                               model_download_phase_{};
  QString                               model_download_current_file_{};
  quint64                               model_download_bytes_done_  = 0;
  quint64                               model_download_bytes_total_ = 0;
  QString                               model_download_bytes_label_{};
  QString                               model_download_speed_label_{};
  QString                               model_download_eta_label_{};
  int                                   model_download_files_done_  = 0;
  int                                   model_download_files_total_ = 0;
  // EMA download-speed tracking. Sampled in the ProgressChanged handler only
  // while phase == "downloading"; the baseline is re-primed whenever download
  // resumes so inter-asset validation gaps don't produce spikes.
  std::chrono::steady_clock::time_point download_sample_time_{};
  quint64                               download_sample_bytes_ = 0;
  double                                download_speed_ema_    = 0.0;
  bool                                  download_speed_primed_ = false;
};

}  // namespace alcedo::ui
