//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/package_archive_download.hpp"

#include <utility>

namespace alcedo {

DownloadServicePackageArchiveDownloader::DownloadServicePackageArchiveDownloader(
    DownloadService& downloads, QObject* parent)
    : QObject(parent), downloads_(downloads) {
  connect(&downloads_, &DownloadService::ProgressChanged, this,
          [this](const DownloadProgress& progress) {
            if (progress.id == active_request_id_ && on_progress_) {
              on_progress_(progress.bytes_downloaded, progress.bytes_total);
            }
          });
  connect(&downloads_, &DownloadService::Finished, this,
          [this](const QString& id, bool ok, bool canceled, const QString& error) {
            if (id != active_request_id_) return;
            FinishedCallback finished = std::move(on_finished_);
            active_request_id_.clear();
            on_progress_ = nullptr;
            on_finished_ = nullptr;
            if (finished) finished(ok, canceled, error);
          });
}

auto DownloadServicePackageArchiveDownloader::Start(const DownloadRequest& request,
                                                    ProgressCallback       on_progress,
                                                    FinishedCallback       on_finished) -> bool {
  if (!downloads_.Start(request)) return false;
  active_request_id_ = request.id;
  on_progress_       = std::move(on_progress);
  on_finished_       = std::move(on_finished);
  return true;
}

void DownloadServicePackageArchiveDownloader::Cancel(const QString& request_id) {
  downloads_.Cancel(request_id);
}

}  // namespace alcedo
