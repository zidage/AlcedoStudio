//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "app/download_service.hpp"
#include "app/model_asset_catalog.hpp"

namespace alcedo {

// Progress for an in-flight model profile download: the byte and file counts
// that ModelDownloadController shows on the row of the profile.
struct ModelDownloadProgress {
  std::string  phase;
  std::string  current_file;
  std::uint64_t bytes_downloaded = 0;
  std::uint64_t bytes_total      = 0;
  std::uint32_t files_completed  = 0;
  std::uint32_t files_total      = 0;
  std::string  message;
};

// Downloads a semantic model profile from Hugging Face through the shared
// DownloadService (aria2c, one job at a time). Reports progress and completion
// through signals on the owner thread. Cancellation removes the active aria2
// download and reports a canceled result.
class ModelDownloadService final : public QObject {
  Q_OBJECT

 public:
  // Downloads the profiles of SemanticModelProfiles().
  explicit ModelDownloadService(DownloadService& downloads, QObject* parent = nullptr);
  // Downloads the given profiles. Tests pass profiles with small files.
  ModelDownloadService(DownloadService& downloads, std::vector<ModelProfileSpec> profiles,
                       QObject* parent = nullptr);
  ~ModelDownloadService() override;

  ModelDownloadService(const ModelDownloadService&)            = delete;
  ModelDownloadService& operator=(const ModelDownloadService&) = delete;

  // The profiles that this service downloads, in catalog order.
  [[nodiscard]] auto Profiles() const -> const std::vector<ModelProfileSpec>& { return profiles_; }
  // Looks up a profile by profile_id or model_id. Returns nullptr if unknown.
  [[nodiscard]] auto FindProfile(const std::string& profile_or_model_id) const
      -> const ModelProfileSpec*;

  // Begins downloading the given profile into model_root (per-profile files are
  // placed under "<model_root>/<profile_id>"). Returns false if a download is
  // already running, the shared DownloadService runs another job, or the profile
  // is unknown. ProgressChanged and Finished are emitted asynchronously.
  auto StartDownload(const std::string& profile_id, const std::filesystem::path& model_root,
                     const std::string& hf_endpoint) -> bool;

  // Requests cancellation of the active download. Safe to call from any thread.
  void CancelDownload();

  [[nodiscard]] auto IsRunning() const -> bool;
  // Profile of the running download; empty when none runs.
  [[nodiscard]] auto RunningProfileId() const -> std::string;

 signals:
  void ProgressChanged(const alcedo::ModelDownloadProgress& progress);
  void Finished(bool ok, const QString& error);

 private:
  DownloadService&              downloads_;
  std::vector<ModelProfileSpec> profiles_;
  QString                       request_id_;
  std::string                   profile_id_;
  std::filesystem::path         model_root_;
  bool                          running_ = false;
};

}  // namespace alcedo

Q_DECLARE_METATYPE(alcedo::ModelDownloadProgress)
