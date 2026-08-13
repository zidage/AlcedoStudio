//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/model_download_service.hpp"

#include "app/model_asset_catalog.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <filesystem>

namespace alcedo {
namespace {

auto ToQString(const std::filesystem::path& path) -> QString {
#ifdef _WIN32
  return QString::fromStdWString(path.wstring());
#else
  return QString::fromStdString(path.string());
#endif
}

auto ValidateDownloadedProfile(const ModelProfileSpec& profile,
                               const std::filesystem::path& staging) -> QString {
  for (const auto& asset : profile.assets) {
    if (const auto error = ValidateAssetFile(asset, staging / asset.local_path);
        error.has_value()) {
      return QString::fromStdString(*error);
    }
  }
  return {};
}

}  // namespace

ModelDownloadService::ModelDownloadService(DownloadService& downloads, QObject* parent)
    : QObject(parent), downloads_(downloads) {
  qRegisterMetaType<alcedo::ModelDownloadProgress>("alcedo::ModelDownloadProgress");
  connect(&downloads_, &DownloadService::ProgressChanged, this,
          [this](const DownloadProgress& progress) {
            if (!running_ || progress.id != request_id_) {
              return;
            }
            ModelDownloadProgress model_progress;
            model_progress.phase            = "downloading";
            model_progress.current_file     = progress.current_file.toStdString();
            model_progress.bytes_downloaded = static_cast<std::uint64_t>(
                std::max<qint64>(0, progress.bytes_downloaded));
            model_progress.bytes_total =
                static_cast<std::uint64_t>(std::max<qint64>(0, progress.bytes_total));
            model_progress.files_completed =
                static_cast<std::uint32_t>(std::max(0, progress.files_completed));
            model_progress.files_total =
                static_cast<std::uint32_t>(std::max(0, progress.files_total));
            model_progress.message = QStringLiteral("downloading %1")
                                         .arg(progress.current_file)
                                         .toStdString();
            emit ProgressChanged(model_progress);
          });
  connect(&downloads_, &DownloadService::Finished, this,
          [this](const QString& id, bool ok, bool canceled, const QString& error) {
            if (!running_ || id != request_id_) {
              return;
            }
            const auto* profile = FindSemanticProfile(profile_id_);
            if (!ok || profile == nullptr) {
              ModelDownloadProgress progress;
              progress.phase = canceled ? "cancelled" : "failed";
              progress.message = error.toStdString();
              emit ProgressChanged(progress);
              running_ = false;
              emit Finished(false, error);
              return;
            }

            const auto staging = StagingRoot(model_root_ / profile->profile_id);
            if (const QString validation_error = ValidateDownloadedProfile(*profile, staging);
                !validation_error.isEmpty()) {
              running_ = false;
              emit Finished(false, validation_error);
              return;
            }
            const auto root = model_root_ / profile->profile_id;
            if (const auto promote_error = PromoteStagingRoot(staging, root);
                promote_error.has_value()) {
              running_ = false;
              emit Finished(false, QString::fromStdString(*promote_error));
              return;
            }
            if (const auto manifest_error = WriteResolvedManifest(*profile, root);
                manifest_error.has_value()) {
              running_ = false;
              emit Finished(false, QString::fromStdString(*manifest_error));
              return;
            }

            ModelDownloadProgress progress;
            progress.phase            = "installed";
            progress.bytes_downloaded = ProfileTotalBytes(*profile);
            progress.bytes_total      = progress.bytes_downloaded;
            progress.files_completed  = static_cast<std::uint32_t>(profile->assets.size());
            progress.files_total      = progress.files_completed;
            progress.message          = "model profile installed";
            emit ProgressChanged(progress);
            running_ = false;
            emit Finished(true, {});
          });
}

ModelDownloadService::~ModelDownloadService() {
  if (running_) {
    downloads_.Cancel(request_id_);
  }
}

auto ModelDownloadService::StartDownload(const std::string& profile_id,
                                         const std::filesystem::path& model_root,
                                         const std::string& hf_endpoint) -> bool {
  if (running_) {
    return false;
  }
  const auto* profile = FindSemanticProfile(profile_id);
  if (profile == nullptr) {
    return false;
  }
  const auto root    = model_root / profile->profile_id;
  const auto staging = StagingRoot(root);
  std::error_code error;
  if (std::filesystem::exists(root, error)
      && ValidateDownloadedProfile(*profile, root).isEmpty()) {
    running_ = true;
    QMetaObject::invokeMethod(
        this,
        [this, profile] {
          ModelDownloadProgress progress;
          progress.phase            = "installed";
          progress.bytes_downloaded = ProfileTotalBytes(*profile);
          progress.bytes_total      = progress.bytes_downloaded;
          progress.files_completed  = static_cast<std::uint32_t>(profile->assets.size());
          progress.files_total      = progress.files_completed;
          progress.message          = "model profile is already installed";
          emit ProgressChanged(progress);
          running_ = false;
          emit Finished(true, {});
        },
        Qt::QueuedConnection);
    return true;
  }

  DownloadRequest request;
  request.id = QStringLiteral("model-")
               + QUuid::createUuid().toString(QUuid::WithoutBraces);
  for (const auto& asset : profile->assets) {
    DownloadItem item;
    item.url             = QUrl(QString::fromStdString(BuildAssetUrl(hf_endpoint, asset)));
    item.destination     = ToQString(staging / asset.local_path);
    item.expected_size   = static_cast<qint64>(asset.size_bytes);
    item.expected_sha256 = asset.sha256 == nullptr
                               ? QByteArray{}
                               : QByteArray::fromHex(QByteArray(asset.sha256));
    request.items.push_back(std::move(item));
  }

  if (!downloads_.Start(request)) {
    return false;
  }
  request_id_ = request.id;
  profile_id_ = profile_id;
  model_root_ = model_root;
  running_    = true;

  ModelDownloadProgress progress;
  progress.phase       = "preparing";
  progress.bytes_total = ProfileTotalBytes(*profile);
  progress.files_total = static_cast<std::uint32_t>(profile->assets.size());
  progress.message     = "preparing model download";
  emit ProgressChanged(progress);
  return true;
}

void ModelDownloadService::CancelDownload() {
  if (running_) {
    downloads_.Cancel(request_id_);
  }
}

auto ModelDownloadService::IsRunning() const -> bool { return running_; }

}  // namespace alcedo
