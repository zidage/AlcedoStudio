//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <functional>

#include "app/download_service.hpp"

namespace alcedo {

/// Archive transfer used by LUT package and AI model package installation.
///
/// The production implementation routes every request through the shared
/// DownloadService, so a package archive and an application or model download never
/// run two transfers at once: Start() returns false while another job runs.
class PackageArchiveDownloader {
 public:
  using ProgressCallback = std::function<void(qint64 bytes_downloaded, qint64 bytes_total)>;
  using FinishedCallback = std::function<void(bool ok, bool canceled, const QString& error)>;

  virtual ~PackageArchiveDownloader()                      = default;
  /// Start @p request; the callbacks run on the caller's thread. Returns false
  /// (and calls nothing) when the transport is busy or the request is invalid.
  virtual auto Start(const DownloadRequest& request, ProgressCallback on_progress,
                     FinishedCallback on_finished) -> bool = 0;
  virtual void Cancel(const QString& request_id)           = 0;
};

/// PackageArchiveDownloader over the process-wide DownloadService. It must not
/// outlive @p downloads.
class DownloadServicePackageArchiveDownloader final : public QObject,
                                                      public PackageArchiveDownloader {
  Q_OBJECT

 public:
  explicit DownloadServicePackageArchiveDownloader(DownloadService& downloads,
                                                   QObject*         parent = nullptr);
  auto Start(const DownloadRequest& request, ProgressCallback on_progress,
             FinishedCallback on_finished) -> bool override;
  void Cancel(const QString& request_id) override;

 private:
  DownloadService& downloads_;
  QString          active_request_id_;
  ProgressCallback on_progress_;
  FinishedCallback on_finished_;
};

}  // namespace alcedo
