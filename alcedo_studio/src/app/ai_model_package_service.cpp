//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_model_package_service.hpp"

#include <QMetaObject>
#include <QSettings>
#include <QVariantMap>
#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

#include "app/package_archive.hpp"

#ifndef ALCEDO_AI_MODEL_FEED_URL
#define ALCEDO_AI_MODEL_FEED_URL ""
#endif
#ifndef ALCEDO_UPDATE_PUBLIC_KEY_BASE64
#define ALCEDO_UPDATE_PUBLIC_KEY_BASE64 ""
#endif

namespace alcedo {
namespace {

namespace fs                               = std::filesystem;

constexpr const char* kTrustedSequenceKey  = "aiModels/packages/highestTrustedSequence";
constexpr const char* kStatementVersionKey = "aiModels/downloadStatementAcceptedVersion";
constexpr const char* kRequestIdPrefix     = "ai-model-package:";

/// A package that this build can install and run, with its supported model set versions.
struct KnownAiModelPackage {
  std::string_view id;
  quint64          minimum_version = 0;
  quint64          maximum_version = 0;
};

constexpr KnownAiModelPackage kKnownPackages[] = {
    {kAiMaskModelPackageId, kAiMaskModelSetMinimumVersion, kAiMaskModelSetMaximumVersion}};

auto BuildPlatform() -> QString {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
  return QStringLiteral("windows-x86_64");
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  return QStringLiteral("macos-arm64");
#else
  return {};
#endif
}

auto ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto PathToQString(const fs::path& path) -> QString {
  const std::string text = PackagePathToUtf8(path);
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto StatusName(AiModelPackageStatus status) -> QString {
  switch (status) {
    case AiModelPackageStatus::kNotInstalled:
      return QStringLiteral("notInstalled");
    case AiModelPackageStatus::kCurrent:
      return QStringLiteral("current");
    case AiModelPackageStatus::kUpdateAvailable:
      return QStringLiteral("updateAvailable");
    case AiModelPackageStatus::kUpdateRequired:
      return QStringLiteral("updateRequired");
    case AiModelPackageStatus::kNeedsNewerApplication:
      return QStringLiteral("needsNewerApplication");
    case AiModelPackageStatus::kRepairRequired:
      return QStringLiteral("repairRequired");
    case AiModelPackageStatus::kDownloading:
      return QStringLiteral("downloading");
    case AiModelPackageStatus::kVerifying:
      return QStringLiteral("verifying");
    case AiModelPackageStatus::kInstalling:
      return QStringLiteral("installing");
    case AiModelPackageStatus::kRemoving:
      return QStringLiteral("removing");
    case AiModelPackageStatus::kError:
      return QStringLiteral("error");
  }
  return {};
}

auto ActionName(AiModelPackageAction action) -> QString {
  switch (action) {
    case AiModelPackageAction::kNone:
      return {};
    case AiModelPackageAction::kInstall:
      return QStringLiteral("install");
    case AiModelPackageAction::kUpdate:
      return QStringLiteral("update");
    case AiModelPackageAction::kRepair:
      return QStringLiteral("repair");
    case AiModelPackageAction::kRetry:
      return QStringLiteral("retry");
  }
  return {};
}

auto IsActionStage(AiModelPackageStatus status) -> bool {
  return status == AiModelPackageStatus::kDownloading ||
         status == AiModelPackageStatus::kVerifying ||
         status == AiModelPackageStatus::kInstalling || status == AiModelPackageStatus::kRemoving;
}

/// True when @p offer is another set than the complete installation @p installed.
auto OffersOtherSet(const AiModelPackageDescriptor& offer, const AiModelPackageArtifact& artifact,
                    const AiModelSetInstallation& installed) -> bool {
  if (offer.model_set_version != installed.model_set_version) {
    return offer.model_set_version > installed.model_set_version;
  }
  return offer.revision.toStdString() != installed.receipt->revision ||
         artifact.inventory_sha256.toHex().toStdString() != installed.receipt->inventory_sha256;
}

/// The version that a removal deletes: the supported installation, else one below the range.
auto RemovableVersion(const AiModelInstallation& installation) -> std::optional<quint64> {
  if (installation.supported) return installation.supported->model_set_version;
  return installation.below_minimum_version;
}

}  // namespace

// ── Preferences and options ─────────────────────────────────────────────────

auto SettingsAiModelPackagePreferences::LoadTrustedSequence() const -> quint64 {
  return QSettings{}.value(QLatin1String(kTrustedSequenceKey), 1).toULongLong();
}

void SettingsAiModelPackagePreferences::SaveTrustedSequence(quint64 sequence) {
  QSettings{}.setValue(QLatin1String(kTrustedSequenceKey), sequence);
}

auto SettingsAiModelPackagePreferences::LoadAcceptedStatementVersion() const -> quint64 {
  return QSettings{}.value(QLatin1String(kStatementVersionKey), 0).toULongLong();
}

void SettingsAiModelPackagePreferences::SaveAcceptedStatementVersion(quint64 version) {
  QSettings{}.setValue(QLatin1String(kStatementVersionKey), version);
}

auto AiModelPackageServiceOptions::FromBuildConfiguration() -> AiModelPackageServiceOptions {
  AiModelPackageServiceOptions options;
  options.feed_url   = QUrl(QStringLiteral(ALCEDO_AI_MODEL_FEED_URL));
  options.public_key = QByteArray::fromBase64(QByteArrayLiteral(ALCEDO_UPDATE_PUBLIC_KEY_BASE64),
                                              QByteArray::AbortOnBase64DecodingErrors);
  options.platform   = BuildPlatform();
  return options;
}

// ── Comparison ───────────────────────────────────────────────────────────────

auto CompareAiModelPackage(const AiModelInstallation&    installation,
                           const AiModelPackageManifest* feed, const QString& package_id,
                           quint64 minimum_version, quint64 maximum_version,
                           const QString& platform) -> AiModelPackageComparison {
  AiModelPackageComparison comparison;
  if (feed != nullptr) {
    comparison.offer =
        SelectAiModelPackage(*feed, package_id, minimum_version, maximum_version, platform);
  }
  if (installation.supported) {
    const AiModelSetInstallation& installed = *installation.supported;
    if (!installed.Complete()) {
      comparison.status = AiModelPackageStatus::kRepairRequired;
    } else if (comparison.offer != nullptr &&
               OffersOtherSet(*comparison.offer, *comparison.offer->Artifact(platform),
                              installed)) {
      comparison.status = AiModelPackageStatus::kUpdateAvailable;
    } else {
      comparison.status = AiModelPackageStatus::kCurrent;
    }
  } else if (installation.below_minimum_version) {
    comparison.status = AiModelPackageStatus::kUpdateRequired;
  } else if (feed != nullptr && comparison.offer == nullptr &&
             ListsAiModelPackageAbove(*feed, package_id, maximum_version, platform)) {
    comparison.status = AiModelPackageStatus::kNeedsNewerApplication;
  } else {
    comparison.status = AiModelPackageStatus::kNotInstalled;
  }
  return comparison;
}

auto AiModelPackageActionFor(AiModelPackageStatus status, bool offered) -> AiModelPackageAction {
  if (!offered) return AiModelPackageAction::kNone;
  switch (status) {
    case AiModelPackageStatus::kNotInstalled:
      return AiModelPackageAction::kInstall;
    case AiModelPackageStatus::kUpdateAvailable:
    case AiModelPackageStatus::kUpdateRequired:
      return AiModelPackageAction::kUpdate;
    case AiModelPackageStatus::kRepairRequired:
      return AiModelPackageAction::kRepair;
    case AiModelPackageStatus::kError:
      return AiModelPackageAction::kRetry;
    case AiModelPackageStatus::kCurrent:
    case AiModelPackageStatus::kNeedsNewerApplication:
    case AiModelPackageStatus::kDownloading:
    case AiModelPackageStatus::kVerifying:
    case AiModelPackageStatus::kInstalling:
    case AiModelPackageStatus::kRemoving:
      return AiModelPackageAction::kNone;
  }
  return AiModelPackageAction::kNone;
}

// ── Lifecycle and reads ─────────────────────────────────────────────────────

AiModelPackageService::AiModelPackageService(AiModelPackageServiceOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
  if (!options_.preferences) {
    options_.preferences = std::make_unique<SettingsAiModelPackagePreferences>();
  }
  if (!options_.fetch) {
    options_.fetch = [this](const QUrl& url, qint64 maximum_bytes,
                            std::function<void(QByteArray, QString)> done) {
      FetchSmallFile(url, maximum_bytes, std::move(done));
    };
  }
  if (options_.feed_url.scheme() != QStringLiteral("https") ||
      !options_.feed_url.userInfo().isEmpty() || options_.public_key.size() != 32 ||
      !options_.downloader || !options_.model_directory) {
    options_.feed_url.clear();
  }
  for (const KnownAiModelPackage& known : kKnownPackages) {
    PackageState package;
    package.id              = ToQString(known.id);
    package.minimum_version = known.minimum_version;
    package.maximum_version = known.maximum_version;
    packages_.push_back(std::move(package));
  }
  // Reading the local receipts makes no network request (plan decision P6).
  RefreshInstallations();
}

AiModelPackageService::~AiModelPackageService() { Shutdown(); }

void AiModelPackageService::Shutdown() {
  if (shut_down_) return;
  shut_down_ = true;
  stop_.store(true);
  if (!active_request_id_.isEmpty() && options_.downloader) {
    options_.downloader->Cancel(active_request_id_);
  }
  JoinWorker();
}

void AiModelPackageService::JoinWorker() {
  if (worker_.joinable()) worker_.join();
}

auto AiModelPackageService::enabled() const -> bool { return options_.feed_url.isValid(); }

auto AiModelPackageService::download_statement_accepted() const -> bool {
  return options_.preferences->LoadAcceptedStatementVersion() >= kAiModelDownloadStatementVersion;
}

auto AiModelPackageService::packages() const -> QVariantList {
  QVariantList list;
  const bool   idle = !ActionRunning() && !checking_;
  for (const PackageState& package : packages_) {
    const AiModelPackageDescriptor* offer    = package.comparison.offer;
    const AiModelPackageArtifact*   artifact = offer ? offer->Artifact(options_.platform) : nullptr;
    const auto&                     installed = package.installation.supported;
    const bool                      active    = package.id == active_package_id_;
    QVariantList                    licenses;
    if (offer != nullptr) {
      for (const AiModelPackageLicense& license : offer->licenses) {
        licenses.push_back(QVariantMap{{QStringLiteral("name"), license.name},
                                       {QStringLiteral("file"), license.file}});
      }
    }
    QString name = offer != nullptr ? offer->name : QString();
    if (name.isEmpty()) {
      name = package.id == ToQString(kAiMaskModelPackageId) ? tr("AI Mask") : package.id;
    }
    list.push_back(QVariantMap{
        {QStringLiteral("id"), package.id},
        {QStringLiteral("name"), name},
        {QStringLiteral("status"), StatusName(package.status)},
        {QStringLiteral("action"),
         idle ? ActionName(AiModelPackageActionFor(package.status, offer != nullptr)) : QString()},
        {QStringLiteral("available"), offer != nullptr},
        {QStringLiteral("modelSetVersion"),
         QVariant::fromValue(offer != nullptr ? offer->model_set_version : 0)},
        {QStringLiteral("revision"), offer != nullptr ? offer->revision : QString()},
        {QStringLiteral("installedModelSetVersion"),
         QVariant::fromValue(installed ? installed->model_set_version
                                       : package.installation.below_minimum_version.value_or(0))},
        {QStringLiteral("installedRevision"),
         installed && installed->receipt ? ToQString(installed->receipt->revision) : QString()},
        {QStringLiteral("minimumSupportedVersion"), QVariant::fromValue(package.minimum_version)},
        {QStringLiteral("maximumSupportedVersion"), QVariant::fromValue(package.maximum_version)},
        {QStringLiteral("archiveBytes"), QVariant::fromValue(artifact ? artifact->size : 0)},
        {QStringLiteral("licenses"), licenses},
        {QStringLiteral("busy"), active},
        {QStringLiteral("cancelable"), active && !removing_},
        {QStringLiteral("removable"), idle && RemovableVersion(package.installation).has_value()},
        {QStringLiteral("progress"), package.progress},
        {QStringLiteral("error"), package.error}});
  }
  return list;
}

auto AiModelPackageService::Package(const QString& package_id) const -> const PackageState* {
  const auto found =
      std::find_if(packages_.begin(), packages_.end(),
                   [&](const PackageState& package) { return package.id == package_id; });
  return found != packages_.end() ? &*found : nullptr;
}

auto AiModelPackageService::MutablePackage(const QString& package_id) -> PackageState* {
  return const_cast<PackageState*>(std::as_const(*this).Package(package_id));
}

auto AiModelPackageService::ArchivePath(const PackageState& package) const -> fs::path {
  const AiModelPackageDescriptor& offer    = *package.comparison.offer;
  const AiModelPackageArtifact&   artifact = *offer.Artifact(options_.platform);
  const std::string               name =
      AiModelPackageDirectoryName(package.id.toStdString(), offer.model_set_version) + "-" +
      offer.revision.toStdString() + "-" + artifact.sha256.toHex().left(16).toStdString() + ".7z";
  return AiModelPackageStagingDirectory(options_.model_directory(), package.id.toStdString(),
                                        offer.model_set_version) /
         PackagePathFromUtf8(name);
}

void AiModelPackageService::RefreshInstallations() {
  if (ActionRunning() || shut_down_) return;
  const fs::path directory = options_.model_directory ? options_.model_directory() : fs::path{};
  for (PackageState& package : packages_) {
    package.installation = ReadAiModelInstallation(
        directory, package.id.toStdString(), package.minimum_version, package.maximum_version);
  }
  Recompare();
}

void AiModelPackageService::Recompare() {
  for (PackageState& package : packages_) {
    package.comparison =
        CompareAiModelPackage(package.installation, manifest_ ? &*manifest_ : nullptr, package.id,
                              package.minimum_version, package.maximum_version, options_.platform);
    if (package.id == active_package_id_) continue;
    package.progress = 0.0;
    package.status =
        package.error.isEmpty() ? package.comparison.status : AiModelPackageStatus::kError;
  }
  emit StateChanged();
}

void AiModelPackageService::SetPackageStatus(const QString&       package_id,
                                             AiModelPackageStatus status) {
  PackageState* package = MutablePackage(package_id);
  if (package == nullptr) return;
  package->status = status;
  emit PackageChanged(package_id);
  emit StateChanged();
}

// ── Feed check ──────────────────────────────────────────────────────────────

auto AiModelPackageService::CheckPackages() -> bool {
  if (!enabled() || checking_ || ActionRunning() || shut_down_) return false;
  // Settings opening is also the point where an interrupted install or removal is finished:
  // the result of such an operation is decided by the file system, not by the feed.
  const fs::path directory = options_.model_directory();
  for (const PackageState& package : packages_) {
    for (quint64 version = package.minimum_version; version <= package.maximum_version; ++version) {
      RecoverAiModelPackageDirectory(directory, package.id.toStdString(), version);
    }
  }
  checking_ = true;
  last_error_.clear();
  for (PackageState& package : packages_) package.error.clear();
  RefreshInstallations();
  FetchSignedFeed(options_.fetch, options_.feed_url,
                  [this](QByteArray manifest, QByteArray signature, QString error) {
                    if (shut_down_) return;
                    if (!error.isEmpty()) {
                      FailCheck(std::move(error));
                      return;
                    }
                    HandleFeed(std::move(manifest), std::move(signature));
                  });
  return true;
}

void AiModelPackageService::FetchSmallFile(const QUrl& url, qint64 maximum_bytes,
                                           std::function<void(QByteArray, QString)> done) {
  FetchFeedFile(network_, this, url, options_.feed_url.host(), maximum_bytes,
                [done = std::move(done)](FeedFileResult result) {
                  QString error;
                  switch (result.error) {
                    case FeedFileError::kNone:
                      break;
                    case FeedFileError::kUrlNotAllowed:
                      error = tr("The AI model server URL is not allowed.");
                      break;
                    case FeedFileError::kTooLarge:
                      error = tr("The AI model server response is too large.");
                      break;
                    case FeedFileError::kNetwork:
                      error = result.network_error;
                      break;
                    case FeedFileError::kHttpStatus:
                      error = tr("The AI model server returned HTTP %1.").arg(result.http_status);
                      break;
                    case FeedFileError::kUntrustedRedirect:
                      error = tr("The AI model server redirected to an untrusted host.");
                      break;
                  }
                  done(std::move(result.bytes), std::move(error));
                });
}

void AiModelPackageService::FailCheck(QString message) {
  checking_   = false;
  last_error_ = std::move(message);
  Recompare();
}

void AiModelPackageService::HandleFeed(QByteArray manifest_bytes, QByteArray signature_text) {
  const quint64                trusted = options_.preferences->LoadTrustedSequence();
  AiModelPackageManifestResult result  = VerifyAiModelPackageManifest(
      manifest_bytes, signature_text, options_.public_key, options_.feed_url, trusted);
  if (!result) {
    FailCheck(std::move(result.error));
    return;
  }
  if (result.manifest->sequence > trusted) {
    options_.preferences->SaveTrustedSequence(result.manifest->sequence);
  }
  checking_ = false;
  manifest_ = std::move(result.manifest);
  Recompare();
}

// ── Download statement ──────────────────────────────────────────────────────

void AiModelPackageService::AcceptDownloadStatement() {
  if (download_statement_accepted()) return;
  options_.preferences->SaveAcceptedStatementVersion(kAiModelDownloadStatementVersion);
  emit StateChanged();
}

// ── Installation ────────────────────────────────────────────────────────────

auto AiModelPackageService::InstallPackage(const QString& package_id) -> bool {
  PackageState* package = MutablePackage(package_id);
  if (package == nullptr || shut_down_ || !enabled()) return false;
  if (!download_statement_accepted()) {
    // The page opens the statement; nothing is downloaded before it is accepted (P8).
    emit DownloadStatementRequired(package_id);
    return false;
  }
  if (ActionRunning() || checking_) {
    package->error = tr("Another AI model action is running.");
    emit PackageChanged(package_id);
    emit StateChanged();
    return false;
  }
  const AiModelPackageDescriptor* offer = package->comparison.offer;
  if (offer == nullptr) return false;
  const AiModelPackageArtifact& artifact = *offer->Artifact(options_.platform);

  AiModelPackageInstallRequest  request;
  request.model_directory            = options_.model_directory();
  request.archive_path               = ArchivePath(*package);
  request.expected.package_id        = package_id.toStdString();
  request.expected.model_set_version = offer->model_set_version;
  request.expected.revision          = offer->revision.toStdString();
  request.expected.platform          = options_.platform.toStdString();
  request.expected.file_count        = artifact.file_count;
  request.expected.unpacked_bytes    = artifact.unpacked_bytes;
  request.expected.inventory_sha256  = artifact.inventory_sha256.toHex().toStdString();
  request.expected.artifact_url      = artifact.url.toString().toStdString();
  request.expected.artifact_size     = static_cast<std::uint64_t>(artifact.size);
  request.expected.artifact_sha256   = artifact.sha256.toHex().toStdString();
  request.expected.feed_sequence     = manifest_->sequence;
  for (const AiModelPackageLicense& license : offer->licenses) {
    request.license_files.push_back(license.file.toStdString());
  }
  std::error_code directory_error;
  fs::create_directories(request.archive_path.parent_path(), directory_error);

  DownloadRequest download;
  download.id = QString::fromLatin1(kRequestIdPrefix) + package_id;
  download.items.push_back(
      {artifact.url, PathToQString(request.archive_path), artifact.size, artifact.sha256});

  active_package_id_ = package_id;
  active_request_id_ = download.id;
  active_request_    = std::move(request);
  package->progress  = 0.0;
  package->error.clear();
  const bool started = options_.downloader->Start(
      download,
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
          StartInstallWorker(package_id);
          return;
        }
        AiModelPackageInstallOutcome outcome;
        outcome.canceled = canceled;
        outcome.error    = canceled ? std::string() : error.toStdString();
        FinishInstall(package_id, std::move(outcome));
      });
  if (!started) {
    // The shared transfer admission is busy (an application, LUT, or model download).
    active_package_id_.clear();
    active_request_id_.clear();
    active_request_.reset();
    package->error = tr("Another download is running. Try again after it finishes.");
    SetPackageStatus(package_id, AiModelPackageStatus::kError);
    return false;
  }
  SetPackageStatus(package_id, AiModelPackageStatus::kDownloading);
  emit InstallStarted(package_id);
  return true;
}

void AiModelPackageService::StartInstallWorker(const QString& package_id) {
  JoinWorker();
  installing_ = true;
  SetPackageStatus(package_id, AiModelPackageStatus::kVerifying);
  worker_ = std::thread([this, package_id, request = *active_request_] {
    AiModelPackageInstallOutcome outcome = InstallAiModelPackageArchive(
        request, stop_, [this, package_id](AiModelPackageInstallStage stage) {
          QMetaObject::invokeMethod(
              this,
              [this, package_id, stage] {
                if (shut_down_ || package_id != active_package_id_) return;
                SetPackageStatus(package_id, stage == AiModelPackageInstallStage::kVerifying
                                                 ? AiModelPackageStatus::kVerifying
                                                 : AiModelPackageStatus::kInstalling);
              },
              Qt::QueuedConnection);
        });
    QMetaObject::invokeMethod(
        this,
        [this, package_id, outcome = std::move(outcome)]() mutable {
          if (shut_down_) return;
          FinishInstall(package_id, std::move(outcome));
        },
        Qt::QueuedConnection);
  });
}

void AiModelPackageService::FinishInstall(const QString&               package_id,
                                          AiModelPackageInstallOutcome outcome) {
  const bool was_installing = installing_;
  installing_               = false;
  stop_.store(false);
  active_package_id_.clear();
  active_request_.reset();
  QString message;
  if (outcome.canceled) {
    message =
        was_installing ? tr("The installation was canceled.") : tr("The download was canceled.");
  } else if (!outcome.committed) {
    message = QString::fromStdString(outcome.error);
  }
  if (PackageState* package = MutablePackage(package_id); package != nullptr) {
    package->error = message;
  }
  RefreshInstallations();
  emit PackageChanged(package_id);
  emit InstallFinished(package_id, outcome.committed, outcome.canceled, message);
}

auto AiModelPackageService::CancelInstall(const QString& package_id) -> bool {
  if (package_id.isEmpty() || package_id != active_package_id_ || removing_) return false;
  if (!active_request_id_.isEmpty()) {
    options_.downloader->Cancel(active_request_id_);
    return true;
  }
  if (installing_) {
    stop_.store(true);
    return true;
  }
  return false;
}

// ── Removal ─────────────────────────────────────────────────────────────────

auto AiModelPackageService::RemovePackage(const QString& package_id) -> bool {
  PackageState* package = MutablePackage(package_id);
  if (package == nullptr || shut_down_) return false;
  if (ActionRunning() || checking_) {
    package->error = tr("Another AI model action is running.");
    emit PackageChanged(package_id);
    emit StateChanged();
    return false;
  }
  const std::optional<quint64> version = RemovableVersion(package->installation);
  if (!version) return false;
  JoinWorker();
  active_package_id_ = package_id;
  removing_          = true;
  package->error.clear();
  package->progress = 0.0;
  SetPackageStatus(package_id, AiModelPackageStatus::kRemoving);
  worker_ =
      std::thread([this, package_id, directory = options_.model_directory(), version = *version] {
        const std::string error =
            RemoveAiModelPackage(directory, package_id.toStdString(), version);
        QMetaObject::invokeMethod(
            this,
            [this, package_id, error] {
              if (shut_down_) return;
              FinishRemoval(package_id, QString::fromStdString(error));
            },
            Qt::QueuedConnection);
      });
  return true;
}

void AiModelPackageService::FinishRemoval(const QString& package_id, QString error) {
  removing_ = false;
  active_package_id_.clear();
  if (PackageState* package = MutablePackage(package_id); package != nullptr) {
    package->error = std::move(error);
  }
  RefreshInstallations();
  emit PackageChanged(package_id);
}

}  // namespace alcedo
