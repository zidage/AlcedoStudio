//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/camera_profile_library_service.hpp"

#include <QDir>
#include <QMetaObject>
#include <filesystem>
#include <limits>
#include <utility>

namespace alcedo {
namespace {

auto ToInt(std::size_t value) -> int {
  return value > static_cast<std::size_t>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(value);
}

}  // namespace

CameraProfileLibraryService::CameraProfileLibraryService(CameraProfileLibraryServiceOptions options,
                                                         QObject*                           parent)
    : QObject(parent),
      options_(std::move(options)),
      library_(options_.library ? options_.library : &DcpProfileLibrary::Shared()) {}

CameraProfileLibraryService::~CameraProfileLibraryService() { Shutdown(); }

void CameraProfileLibraryService::Shutdown() {
  shut_down_ = true;
  worker_.Shutdown();
}

auto CameraProfileLibraryService::Settings() const -> std::unique_ptr<QSettings> {
  if (options_.settings_file.isEmpty()) return std::make_unique<QSettings>();
  return std::make_unique<QSettings>(options_.settings_file, QSettings::IniFormat);
}

void CameraProfileLibraryService::Start() {
  root_path_ = Settings()->value(QString::fromLatin1(kRootSettingsKey)).toString();
  emit RootChanged();
  if (!root_path_.isEmpty()) StartScan();
}

void CameraProfileLibraryService::setRootFolder(const QUrl& folder) {
  if (shut_down_) return;
  const QString path =
      folder.isEmpty() ? QString() : QDir::toNativeSeparators(folder.toLocalFile());
  auto settings = Settings();
  settings->setValue(QString::fromLatin1(kRootSettingsKey), path);
  settings->sync();
  if (settings->status() != QSettings::NoError) {
    error_text_ = tr("The DCP folder setting cannot be saved.");
    emit ScanResultChanged();
    return;
  }
  if (path != root_path_) {
    root_path_ = path;
    emit RootChanged();
  }
  StartScan();
}

void CameraProfileLibraryService::rescan() {
  if (shut_down_ || root_path_.isEmpty()) return;
  StartScan();
}

void CameraProfileLibraryService::StartScan() {
  if (++pending_scans_ == 1) emit ScanningChanged();
  const std::filesystem::path root(root_path_.toStdWString());
  worker_.Submit([this, library = library_, root] {
    const DcpLibraryScanResult result = library->Scan(root);
    // Destroying this object drops the posted call; Shutdown drops it through shut_down_.
    QMetaObject::invokeMethod(
        this,
        [this, result] {
          if (!shut_down_) PublishScanResult(result);
        },
        Qt::QueuedConnection);
  });
}

void CameraProfileLibraryService::PublishScanResult(const DcpLibraryScanResult& result) {
  profile_count_  = ToInt(result.profile_count);
  camera_count_   = ToInt(result.camera_count);
  rejected_count_ = ToInt(result.rejected_count);
  // std::error_code text is in the narrow encoding of the operating system.
  error_text_     = QString::fromLocal8Bit(result.error);
  emit ScanResultChanged();
  if (--pending_scans_ == 0) emit ScanningChanged();
  emit ScanFinished();
}

}  // namespace alcedo
