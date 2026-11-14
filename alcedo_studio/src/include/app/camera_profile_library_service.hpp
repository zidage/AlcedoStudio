//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <memory>

#include "concurrency/thread_pool.hpp"
#include "image/dcp_profile_library.hpp"

namespace alcedo {

struct CameraProfileLibraryServiceOptions {
  /// Library that the scans fill. Null selects DcpProfileLibrary::Shared().
  DcpProfileLibrary* library = nullptr;
  /// INI file that holds the folder setting. Empty selects the application QSettings.
  QString            settings_file;
};

/**
 * @brief Owner of the DCP library folder setting and of the scans of that folder.
 *
 * The folder path is the only persisted value (QSettings key @ref kRootSettingsKey). Each folder
 * change and each @ref rescan starts one DcpProfileLibrary::Scan on the worker thread; the scan
 * result is published on the owner thread as the count and error properties. Scans run one after
 * another in request order, so the last result is the result of the last request.
 *
 * Thread affinity: every public method runs on the thread that owns this object (the GUI
 * thread). The worker touches only the library, which has its own lock.
 */
class CameraProfileLibraryService final : public QObject {
  Q_OBJECT
  /// Native path of the DCP folder; empty when no folder is set.
  Q_PROPERTY(QString rootPath READ root_path NOTIFY RootChanged)
  Q_PROPERTY(int profileCount READ profile_count NOTIFY ScanResultChanged)
  Q_PROPERTY(int cameraCount READ camera_count NOTIFY ScanResultChanged)
  Q_PROPERTY(int rejectedCount READ rejected_count NOTIFY ScanResultChanged)
  /// Operating system text of a folder error of the last scan, or a setting write error.
  Q_PROPERTY(QString errorText READ error_text NOTIFY ScanResultChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY ScanningChanged)

 public:
  static constexpr const char* kRootSettingsKey = "cameraProfiles/dcpLibraryRoot";

  explicit CameraProfileLibraryService(CameraProfileLibraryServiceOptions options = {},
                                       QObject*                           parent  = nullptr);
  ~CameraProfileLibraryService() override;

  /// Read the folder setting and start one scan when it is not empty.
  void               Start();
  /// Stop the worker. A running scan completes; its result is not published. Idempotent.
  void               Shutdown();

  /**
   * @brief Store @p folder as the DCP folder and scan it.
   *
   * An empty URL clears the folder and the counts. The stored path stays when the folder cannot
   * be read, so that a removable drive works after it returns.
   */
  Q_INVOKABLE void   setRootFolder(const QUrl& folder);
  /// Scan the stored folder again. No effect when no folder is set.
  Q_INVOKABLE void   rescan();

  [[nodiscard]] auto root_path() const -> QString { return root_path_; }
  [[nodiscard]] auto profile_count() const -> int { return profile_count_; }
  [[nodiscard]] auto camera_count() const -> int { return camera_count_; }
  [[nodiscard]] auto rejected_count() const -> int { return rejected_count_; }
  [[nodiscard]] auto error_text() const -> QString { return error_text_; }
  [[nodiscard]] auto scanning() const -> bool { return pending_scans_ > 0; }
  [[nodiscard]] auto library() -> DcpProfileLibrary& { return *library_; }

 signals:
  void RootChanged();
  void ScanResultChanged();
  void ScanningChanged();
  /// One scan request completed and its result is published.
  void ScanFinished();

 private:
  [[nodiscard]] auto                 Settings() const -> std::unique_ptr<QSettings>;
  void                               StartScan();
  void                               PublishScanResult(const DcpLibraryScanResult& result);

  CameraProfileLibraryServiceOptions options_;
  DcpProfileLibrary*                 library_;
  QString                            root_path_;
  int                                profile_count_  = 0;
  int                                camera_count_   = 0;
  int                                rejected_count_ = 0;
  QString                            error_text_;
  int                                pending_scans_ = 0;
  bool                               shut_down_     = false;
  /// One thread, so scans run in request order. Declared last: it stops before the state above.
  ThreadPool                         worker_{1};
};

}  // namespace alcedo
