//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>

namespace alcedo {

/// One file in an aria2c-backed download job.
struct DownloadItem {
  QUrl       url;
  QString    destination;
  qint64     expected_size = 0;
  QByteArray expected_sha256;
};

/// A caller-owned logical download. DownloadService serializes jobs so model
/// assets and application packages never start competing aria2c daemons.
struct DownloadRequest {
  QString             id;
  QList<DownloadItem> items;
};

/// Transport metrics reported by aria2c. Callers remain responsible for their
/// own post-download operations, such as model promotion or package staging.
struct DownloadProgress {
  QString id;
  QString current_file;
  qint64  bytes_downloaded = 0;
  qint64  bytes_total      = 0;
  qint64  bytes_per_second = 0;
  int     files_completed  = 0;
  int     files_total      = 0;
};

/// Shared process-wide aria2c download service.
class DownloadService final : public QObject {
  Q_OBJECT

 public:
  explicit DownloadService(QObject* parent = nullptr);
  ~DownloadService() override;

  DownloadService(const DownloadService&)            = delete;
  DownloadService& operator=(const DownloadService&) = delete;

  /// Starts a job asynchronously. Returns false when another job is active or
  /// when the request is malformed.
  auto Start(const DownloadRequest& request) -> bool;
  void Cancel(const QString& id);

  [[nodiscard]] auto IsRunning() const -> bool;
  [[nodiscard]] auto ActiveRequestId() const -> QString;

 signals:
  void ProgressChanged(const alcedo::DownloadProgress& progress);
  void Finished(const QString& id, bool ok, bool canceled, const QString& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

Q_DECLARE_METATYPE(alcedo::DownloadItem)
Q_DECLARE_METATYPE(alcedo::DownloadRequest)
Q_DECLARE_METATYPE(alcedo::DownloadProgress)
