//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "app/download_service.hpp"

namespace alcedo {

// Progress for an in-flight model profile download. Mirrors the byte/file
// contract the QML layer already consumes through SemanticGenerationController.
struct ModelDownloadProgress {
  std::string  phase;
  std::string  current_file;
  std::uint64_t bytes_downloaded = 0;
  std::uint64_t bytes_total      = 0;
  std::uint32_t files_completed  = 0;
  std::uint32_t files_total      = 0;
  std::string  message;
};

// Downloads a semantic model profile from Hugging Face using an aria2c
// JSON-RPC daemon (one daemon per download). Runs the orchestration on a
// dedicated worker thread and reports progress/completion via signals that are
// safe to connect from the UI thread. Cancellation is cooperative: it removes
// the active aria2 download, shuts the daemon down, and reports a cancelled
// result.
class ModelDownloadService final : public QObject {
  Q_OBJECT

 public:
  explicit ModelDownloadService(DownloadService& downloads, QObject* parent = nullptr);
  ~ModelDownloadService() override;

  ModelDownloadService(const ModelDownloadService&)            = delete;
  ModelDownloadService& operator=(const ModelDownloadService&) = delete;

  // Begins downloading the given profile into model_root (per-profile files are
  // placed under "<model_root>/<profile_id>"). Returns false if a download is
  // already running or the profile is unknown. ProgressChanged and Finished are
  // emitted asynchronously.
  auto StartDownload(const std::string& profile_id, const std::filesystem::path& model_root,
                     const std::string& hf_endpoint) -> bool;

  // Requests cancellation of the active download. Safe to call from any thread.
  void CancelDownload();

  [[nodiscard]] auto IsRunning() const -> bool;

 signals:
  void ProgressChanged(const alcedo::ModelDownloadProgress& progress);
  void Finished(bool ok, const QString& error);

 private:
  DownloadService& downloads_;
  QString          request_id_;
  std::string      profile_id_;
  std::filesystem::path model_root_;
  bool             running_ = false;
};

}  // namespace alcedo

Q_DECLARE_METATYPE(alcedo::ModelDownloadProgress)
