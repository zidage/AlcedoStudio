//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_package_service.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QVariantMap>
#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>

#include "utils/lut/lut_inventory_digest.hpp"

#ifndef ALCEDO_LUT_PACKAGE_FEED_URL
#define ALCEDO_LUT_PACKAGE_FEED_URL ""
#endif
#ifndef ALCEDO_UPDATE_PUBLIC_KEY_BASE64
#define ALCEDO_UPDATE_PUBLIC_KEY_BASE64 ""
#endif

namespace alcedo {
namespace {

namespace fs                                 = std::filesystem;

constexpr qint64      kMaximumFeedBytes      = 256 * 1024;
constexpr qint64      kMaximumSignatureBytes = 1024;
constexpr const char* kTrustedSequenceKey    = "lut/packages/highestTrustedSequence";
constexpr const char* kRequestIdPrefix       = "lut-package:";

auto                  ToQString(const fs::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                            static_cast<qsizetype>(text.size()));
}

auto StatusName(LutPackageStatus status) -> QString {
  switch (status) {
    case LutPackageStatus::kNotInstalled:
      return QStringLiteral("notInstalled");
    case LutPackageStatus::kCurrent:
      return QStringLiteral("current");
    case LutPackageStatus::kUpdateAvailable:
      return QStringLiteral("updateAvailable");
    case LutPackageStatus::kRepairRequired:
      return QStringLiteral("repairRequired");
    case LutPackageStatus::kChecking:
      return QStringLiteral("checking");
    case LutPackageStatus::kDownloading:
      return QStringLiteral("downloading");
    case LutPackageStatus::kVerifying:
      return QStringLiteral("verifying");
    case LutPackageStatus::kInstalling:
      return QStringLiteral("installing");
    case LutPackageStatus::kError:
      return QStringLiteral("error");
  }
  return {};
}

/// Expected receipt fields of a verified descriptor; the installation chooses
/// the content directory.
auto ExpectedReceipt(const LutPackageDescriptor& descriptor, quint64 sequence)
    -> LutPackageReceipt {
  LutPackageReceipt receipt;
  receipt.package_id       = descriptor.id.toStdString();
  receipt.revision         = descriptor.revision.toStdString();
  receipt.file_count       = descriptor.file_count;
  receipt.inventory_sha256 = descriptor.inventory_sha256.toHex().toStdString();
  receipt.unpacked_bytes   = descriptor.unpacked_bytes;
  receipt.artifact_url     = descriptor.artifact.url.toString().toStdString();
  receipt.artifact_size    = static_cast<std::uint64_t>(descriptor.artifact.size);
  receipt.artifact_sha256  = descriptor.artifact.sha256.toHex().toStdString();
  receipt.feed_sequence    = sequence;
  return receipt;
}

auto NewFeedRequest(const QUrl& url) -> QNetworkRequest {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(15000);
  request.setRawHeader("Accept", "application/json, text/plain;q=0.9");
  request.setRawHeader("Cache-Control", "no-cache");
  return request;
}

}  // namespace

// ── Preferences, transport, and options ──────────────────────────────────────

auto SettingsLutPackagePreferences::LoadTrustedSequence() const -> quint64 {
  return QSettings{}.value(QLatin1String(kTrustedSequenceKey), 1).toULongLong();
}

void SettingsLutPackagePreferences::SaveTrustedSequence(quint64 sequence) {
  QSettings{}.setValue(QLatin1String(kTrustedSequenceKey), sequence);
}

DownloadServiceLutArchiveDownloader::DownloadServiceLutArchiveDownloader(DownloadService& downloads,
                                                                         QObject*         parent)
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

auto DownloadServiceLutArchiveDownloader::Start(const DownloadRequest& request,
                                                ProgressCallback       on_progress,
                                                FinishedCallback       on_finished) -> bool {
  if (!downloads_.Start(request)) return false;
  active_request_id_ = request.id;
  on_progress_       = std::move(on_progress);
  on_finished_       = std::move(on_finished);
  return true;
}

void DownloadServiceLutArchiveDownloader::Cancel(const QString& request_id) {
  downloads_.Cancel(request_id);
}

auto LutPackageServiceOptions::FromBuildConfiguration() -> LutPackageServiceOptions {
  LutPackageServiceOptions options;
  options.feed_url   = QUrl(QStringLiteral(ALCEDO_LUT_PACKAGE_FEED_URL));
  options.public_key = QByteArray::fromBase64(QByteArrayLiteral(ALCEDO_UPDATE_PUBLIC_KEY_BASE64),
                                              QByteArray::AbortOnBase64DecodingErrors);
  return options;
}

// ── Comparison ───────────────────────────────────────────────────────────────

auto CompareLutPackage(const LutPackageDescriptor& descriptor, const LutLibraryService& library)
    -> LutPackageComparison {
  LutPackageComparison comparison;
  comparison.local_verification_complete = library.inventory_complete();
  const std::string package_id           = descriptor.id.toStdString();
  const auto&       receipts             = library.PackageReceipts();
  const auto        receipt = std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
    return item.package_id == package_id;
  });
  if (receipt == receipts.end()) {
    comparison.status = LutPackageStatus::kNotInstalled;
    return comparison;
  }

  // Rebuild the canonical records from the published inventory; no file is read.
  const std::string               prefix = receipt->content_directory + "/";
  std::vector<LutInventoryRecord> records;
  bool                            representable = true;
  library.ForEachPackageEntry(package_id, [&](const LutLibraryEntry& entry) {
    ++comparison.local_file_count;
    if (!entry.IsOfficial() || entry.sha256.empty() || !entry.relative_path.starts_with(prefix)) {
      // A file that no longer declares official metadata cannot match a signed record.
      representable = false;
      return;
    }
    records.push_back({entry.header.metadata->id, entry.relative_path.substr(prefix.size()),
                       entry.size, entry.sha256});
  });
  if (representable) {
    LutInventoryDigestResult digest = ComputeLutInventoryDigest(std::move(records));
    if (digest.Ok()) comparison.local_inventory_sha256 = std::move(digest.sha256);
  }

  const std::string remote_digest = descriptor.inventory_sha256.toHex().toStdString();
  if (receipt->inventory_sha256 != remote_digest) {
    comparison.status = LutPackageStatus::kUpdateAvailable;
  } else if (comparison.local_inventory_sha256 == remote_digest &&
             comparison.local_file_count == descriptor.file_count) {
    comparison.status = LutPackageStatus::kCurrent;
  } else {
    comparison.status = LutPackageStatus::kRepairRequired;
  }
  return comparison;
}

// ── Lifecycle and reads ─────────────────────────────────────────────────────

LutPackageService::LutPackageService(LutPackageServiceOptions options, LutLibraryService& library,
                                     QObject* parent)
    : QObject(parent), options_(std::move(options)), library_(library) {
  if (!options_.preferences) {
    options_.preferences = std::make_unique<SettingsLutPackagePreferences>();
  }
  if (!options_.fetch) {
    options_.fetch = [this](const QUrl& url, qint64 maximum_bytes,
                            std::function<void(QByteArray, QString)> done) {
      FetchSmallFile(url, maximum_bytes, std::move(done));
    };
  }
  if (options_.feed_url.scheme() != QStringLiteral("https") ||
      !options_.feed_url.userInfo().isEmpty() || options_.public_key.size() != 32 ||
      !options_.downloader) {
    options_.feed_url.clear();
  }
  connect(&library_, &LutLibraryService::InventoryChanged, this, [this] {
    if (checked()) RecomputeComparisons();
  });
  connect(&library_, &LutLibraryService::OperationFinished, this,
          &LutPackageService::OnLibraryOperationFinished);
  connect(&library_, &LutLibraryService::PackageInstallStageChanged, this,
          [this](const QString& package_id, LutPackageInstallStage stage) {
            if (package_id != active_package_id_ || !activating_) return;
            SetPackageStatus(package_id, stage == LutPackageInstallStage::kVerifying
                                             ? LutPackageStatus::kVerifying
                                             : LutPackageStatus::kInstalling);
          });
}

LutPackageService::~LutPackageService() { Shutdown(); }

void LutPackageService::Shutdown() {
  if (shut_down_) return;
  shut_down_ = true;
  if (!active_request_id_.isEmpty() && options_.downloader) {
    options_.downloader->Cancel(active_request_id_);
  }
}

auto LutPackageService::enabled() const -> bool { return options_.feed_url.isValid(); }

auto LutPackageService::packages() const -> QVariantList {
  QVariantList list;
  for (const PackageState& package : packages_) {
    const auto& receipts = library_.PackageReceipts();
    const auto  receipt  = std::find_if(receipts.begin(), receipts.end(), [&](const auto& item) {
      return item.package_id == package.descriptor.id.toStdString();
    });
    list.push_back(QVariantMap{
        {QStringLiteral("id"), package.descriptor.id},
        {QStringLiteral("revision"), package.descriptor.revision},
        {QStringLiteral("installedRevision"),
         receipt != receipts.end() ? QString::fromStdString(receipt->revision) : QString()},
        {QStringLiteral("fileCount"), QVariant::fromValue(package.descriptor.file_count)},
        {QStringLiteral("archiveBytes"), QVariant::fromValue(package.descriptor.artifact.size)},
        {QStringLiteral("status"), StatusName(package.status)},
        {QStringLiteral("localVerificationComplete"),
         package.comparison.local_verification_complete},
        {QStringLiteral("progress"), package.progress},
        {QStringLiteral("error"), package.error}});
  }
  return list;
}

auto LutPackageService::Package(const QString& package_id) const -> const PackageState* {
  const auto found = std::find_if(packages_.begin(), packages_.end(), [&](const auto& package) {
    return package.descriptor.id == package_id;
  });
  return found != packages_.end() ? &*found : nullptr;
}

auto LutPackageService::MutablePackage(const QString& package_id) -> PackageState* {
  return const_cast<PackageState*>(std::as_const(*this).Package(package_id));
}

auto LutPackageService::ArchivePath(const PackageState& package) const -> fs::path {
  const QString name = package.descriptor.id + QLatin1Char('-') +
                       QString::fromLatin1(package.descriptor.artifact.sha256.toHex().left(16)) +
                       QStringLiteral(".7z");
  return library_.Root() / LutPathFromUtf8(kLutDownloadsDirectoryName) /
         LutPathFromUtf8(name.toStdString());
}

// ── Feed check ──────────────────────────────────────────────────────────────

auto LutPackageService::CheckPackages() -> bool {
  if (!enabled() || checking_ || ActionRunning() || shut_down_) return false;
  checking_ = true;
  last_error_.clear();
  for (PackageState& package : packages_) {
    package.status = LutPackageStatus::kChecking;
    package.error.clear();
  }
  emit StateChanged();
  options_.fetch(options_.feed_url, kMaximumFeedBytes, [this](QByteArray bytes, QString error) {
    if (shut_down_) return;
    if (!error.isEmpty()) {
      FailCheck(std::move(error));
      return;
    }
    QUrl signature_url = options_.feed_url;
    signature_url.setPath(signature_url.path() + QStringLiteral(".sig"));
    options_.fetch(signature_url, kMaximumSignatureBytes,
                   [this, bytes = std::move(bytes)](QByteArray signature, QString error) mutable {
                     if (shut_down_) return;
                     if (!error.isEmpty()) {
                       FailCheck(std::move(error));
                       return;
                     }
                     HandleFeed(std::move(bytes), std::move(signature));
                   });
  });
  return true;
}

void LutPackageService::FetchSmallFile(const QUrl& url, qint64 maximum_bytes,
                                       std::function<void(QByteArray, QString)> done) {
  const QString feed_host = options_.feed_url.host();
  if (url.scheme() != QStringLiteral("https") ||
      url.host().compare(feed_host, Qt::CaseInsensitive) != 0 || !url.userInfo().isEmpty()) {
    done({}, tr("The LUT package server URL is not allowed."));
    return;
  }
  QNetworkReply* reply   = network_.get(NewFeedRequest(url));
  auto           payload = std::make_shared<QByteArray>();
  connect(reply, &QNetworkReply::readyRead, this, [reply, payload, maximum_bytes] {
    payload->append(reply->readAll());
    if (payload->size() > maximum_bytes) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, payload, maximum_bytes, feed_host, done = std::move(done)] {
            payload->append(reply->readAll());
            QString   error;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (payload->size() > maximum_bytes) {
              error = tr("The LUT package server response is too large.");
            } else if (reply->error() != QNetworkReply::NoError) {
              error = reply->errorString();
            } else if (status != 200) {
              error = tr("The LUT package server returned HTTP %1.").arg(status);
            } else if (reply->url().scheme() != QStringLiteral("https") ||
                       reply->url().host().compare(feed_host, Qt::CaseInsensitive) != 0) {
              error = tr("The LUT package server redirected to an untrusted host.");
            }
            reply->deleteLater();
            done(error.isEmpty() ? std::move(*payload) : QByteArray{}, std::move(error));
          });
}

void LutPackageService::FailCheck(QString message) {
  checking_   = false;
  last_error_ = std::move(message);
  RecomputeComparisons();
  emit StateChanged();
}

void LutPackageService::HandleFeed(QByteArray manifest_bytes, QByteArray signature_text) {
  const quint64            trusted = options_.preferences->LoadTrustedSequence();
  LutPackageManifestResult result  = VerifyLutPackageManifest(
      manifest_bytes, signature_text, options_.public_key, options_.feed_url, trusted);
  if (!result) {
    FailCheck(std::move(result.error));
    return;
  }
  if (result.manifest->sequence > trusted) {
    options_.preferences->SaveTrustedSequence(result.manifest->sequence);
  }
  checking_       = false;
  feed_sequence_  = result.manifest->sequence;
  feed_manifest_  = std::move(manifest_bytes);
  feed_signature_ = std::move(signature_text);
  packages_.clear();
  for (const LutPackageDescriptor& descriptor : result.manifest->packages) {
    packages_.push_back(PackageState{.descriptor = descriptor});
  }
  RecomputeComparisons();
  // An incomplete local inventory is not proof of on-disk integrity; verify it.
  if (!library_.inventory_complete()) library_.RefreshInventory();
  emit StateChanged();
}

void LutPackageService::RecomputeComparisons() {
  for (PackageState& package : packages_) {
    package.comparison = CompareLutPackage(package.descriptor, library_);
    if (package.descriptor.id == active_package_id_) continue;
    package.progress = 0.0;
    package.status = package.error.isEmpty() ? package.comparison.status : LutPackageStatus::kError;
  }
  emit StateChanged();
}

void LutPackageService::SetPackageStatus(const QString& package_id, LutPackageStatus status,
                                         QString error) {
  PackageState* package = MutablePackage(package_id);
  if (package == nullptr) return;
  package->status = status;
  package->error  = std::move(error);
  emit PackageChanged(package_id);
  emit StateChanged();
}

// ── Installation ────────────────────────────────────────────────────────────

auto LutPackageService::InstallPackage(const QString& package_id) -> bool {
  PackageState* package = MutablePackage(package_id);
  if (package == nullptr || shut_down_) return false;
  if (ActionRunning() || checking_) {
    package->error = tr("Another LUT package action is running.");
    emit PackageChanged(package_id);
    emit StateChanged();
    return false;
  }
  const fs::path  archive = ArchivePath(*package);
  DownloadRequest request;
  request.id = QString::fromLatin1(kRequestIdPrefix) + package_id;
  request.items.push_back({package->descriptor.artifact.url, ToQString(archive),
                           package->descriptor.artifact.size, package->descriptor.artifact.sha256});

  active_package_id_ = package_id;
  active_request_id_ = request.id;
  package->progress  = 0.0;
  package->error.clear();
  const bool started = options_.downloader->Start(
      request,
      [this, package_id](qint64 downloaded, qint64 total) {
        PackageState* current = MutablePackage(package_id);
        if (shut_down_ || current == nullptr || total <= 0) return;
        current->progress =
            std::clamp(static_cast<double>(downloaded) / static_cast<double>(total), 0.0, 1.0);
        emit PackageChanged(package_id);
        emit StateChanged();
      },
      [this, package_id](bool ok, bool canceled, const QString& error) {
        if (shut_down_) return;
        active_request_id_.clear();
        if (ok) {
          StartActivation(package_id);
          return;
        }
        active_package_id_.clear();
        if (PackageState* current = MutablePackage(package_id); current != nullptr) {
          current->error = canceled ? tr("The download was canceled.") : error;
        }
        RecomputeComparisons();
      });
  if (!started) {
    // The shared transfer admission is busy (an application or model download).
    active_package_id_.clear();
    active_request_id_.clear();
    SetPackageStatus(package_id, LutPackageStatus::kError,
                     tr("Another download is running. Try again after it finishes."));
    return false;
  }
  SetPackageStatus(package_id, LutPackageStatus::kDownloading);
  return true;
}

void LutPackageService::StartActivation(const QString& package_id) {
  const PackageState* package = Package(package_id);
  if (package == nullptr) {
    active_package_id_.clear();
    RecomputeComparisons();
    return;
  }
  LutPackageInstallRequest request;
  request.archive_path                   = ArchivePath(*package);
  request.expected                       = ExpectedReceipt(package->descriptor, feed_sequence_);
  request.feed_manifest                  = feed_manifest_.toStdString();
  request.feed_signature                 = feed_signature_.toStdString();
  const LutLibraryService::Status status = library_.InstallPackage(std::move(request));
  if (status == LutLibraryService::Status::kOk) {
    activation_pending_ = false;
    activating_         = true;
    SetPackageStatus(package_id, LutPackageStatus::kVerifying);
    return;
  }
  if (status == LutLibraryService::Status::kBusy) {
    // A library operation runs; activation starts when it finishes.
    activation_pending_ = true;
    SetPackageStatus(package_id, LutPackageStatus::kVerifying);
    return;
  }
  active_package_id_ = QString();
  SetPackageStatus(package_id, LutPackageStatus::kError,
                   tr("The LUT package cannot be installed now."));
}

void LutPackageService::OnLibraryOperationFinished(LutLibraryService::Operation operation,
                                                   LutLibraryService::Status    status) {
  if (shut_down_) return;
  if (activation_pending_ && !activating_) {
    StartActivation(active_package_id_);
    return;
  }
  if (operation != LutLibraryService::Operation::kInstallPackage || !activating_) return;
  activating_              = false;
  const QString package_id = active_package_id_;
  active_package_id_.clear();
  const LutLibraryService::OperationResult& result = library_.LastResult();
  if (PackageState* package = MutablePackage(package_id); package != nullptr) {
    if (status == LutLibraryService::Status::kCanceled) {
      // Verified bytes may be reused by a retry; the transfer resumes or is skipped.
      package->error = tr("The installation was canceled.");
    } else {
      std::error_code ignored;
      fs::remove(ArchivePath(*package), ignored);
      package->error = status == LutLibraryService::Status::kOk
                           ? QString()
                           : QString::fromStdString(result.message);
    }
  }
  RecomputeComparisons();
}

auto LutPackageService::CancelInstall(const QString& package_id) -> bool {
  if (package_id.isEmpty() || package_id != active_package_id_) return false;
  if (!active_request_id_.isEmpty()) {
    options_.downloader->Cancel(active_request_id_);
    return true;
  }
  if (activating_) return library_.CancelOperation();
  if (activation_pending_) {
    activation_pending_ = false;
    active_package_id_.clear();
    if (PackageState* package = MutablePackage(package_id); package != nullptr) {
      package->error = tr("The installation was canceled.");
    }
    RecomputeComparisons();
    return true;
  }
  return false;
}

}  // namespace alcedo
