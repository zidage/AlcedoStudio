//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "app/ai_model_package_install.hpp"
#include "app/ai_model_package_manifest.hpp"
#include "app/package_archive_download.hpp"
#include "app/signed_feed_fetch.hpp"

namespace alcedo {

/// Version of the download statement text (plan section 4). A text change that changes its
/// meaning raises it, so the user accepts the new text before the next download.
inline constexpr quint64 kAiModelDownloadStatementVersion = 1;

/// Persistent AI model feed trust state and the accepted download statement version.
class AiModelPackagePreferences {
 public:
  virtual ~AiModelPackagePreferences()                                       = default;
  /// Highest feed sequence accepted so far; 1 before the first check.
  [[nodiscard]] virtual auto LoadTrustedSequence() const -> quint64          = 0;
  virtual void               SaveTrustedSequence(quint64 sequence)           = 0;
  /// Highest accepted statement version; 0 before the first acceptance.
  [[nodiscard]] virtual auto LoadAcceptedStatementVersion() const -> quint64 = 0;
  virtual void               SaveAcceptedStatementVersion(quint64 version)   = 0;
};

/// QSettings-backed preferences (`aiModels/packages/highestTrustedSequence`,
/// `aiModels/downloadStatementAcceptedVersion`).
class SettingsAiModelPackagePreferences final : public AiModelPackagePreferences {
 public:
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override;
  void               SaveTrustedSequence(quint64 sequence) override;
  [[nodiscard]] auto LoadAcceptedStatementVersion() const -> quint64 override;
  void               SaveAcceptedStatementVersion(quint64 version) override;
};

struct AiModelPackageServiceOptions {
  /// Signed feed URL (`ALCEDO_AI_MODEL_FEED_URL`). Empty disables the feed and downloads.
  QUrl                                       feed_url;
  /// 32-byte Ed25519 public key (the software-update key).
  QByteArray                                 public_key;
  /// Platform key of the artifacts this build installs (`windows-x86_64`, `macos-arm64`).
  /// Empty: no artifact matches, so nothing can be installed.
  QString                                    platform;
  /// The AI model directory (`semantic/modelDirectory`). Required.
  std::function<std::filesystem::path()>     model_directory;
  /// Defaults to an HTTPS GET restricted to the feed host.
  FeedFileFetch                              fetch;
  /// Required for downloads.
  std::unique_ptr<PackageArchiveDownloader>  downloader;
  /// Defaults to SettingsAiModelPackagePreferences.
  std::unique_ptr<AiModelPackagePreferences> preferences;

  /// Feed URL, key, and platform from this build's compile definitions.
  [[nodiscard]] static auto FromBuildConfiguration() -> AiModelPackageServiceOptions;
};

/// Per-package state shown by the AI Models page (plan decision P5).
enum class AiModelPackageStatus {
  kNotInstalled,
  kCurrent,
  /// A newer revision of the installed version, or a higher supported version, is listed.
  kUpdateAvailable,
  /// Only a version below this build's minimum is installed.
  kUpdateRequired,
  /// The feed lists only versions above this build's maximum and no supported set is
  /// installed.
  kNeedsNewerApplication,
  /// The installed set does not agree with its inventory.
  kRepairRequired,
  kDownloading,
  kVerifying,
  kInstalling,
  kRemoving,
  kError
};

/// The one package action the page offers for a package row.
enum class AiModelPackageAction { kNone, kInstall, kUpdate, kRepair, kRetry };

/// Comparison of the verified feed (if checked) with the local installation.
struct AiModelPackageComparison {
  AiModelPackageStatus            status = AiModelPackageStatus::kNotInstalled;
  /// The feed entry that an install, update, or repair downloads: the highest listed version
  /// in the supported range with an artifact for this platform. Points into the compared feed.
  const AiModelPackageDescriptor* offer  = nullptr;
};

/// Compare @p installation with @p feed (nullptr before a successful check). Reads the
/// receipts and the feed only; no file is hashed. Pure function.
[[nodiscard]] auto CompareAiModelPackage(const AiModelInstallation&    installation,
                                         const AiModelPackageManifest* feed,
                                         const QString& package_id, quint64 minimum_version,
                                         quint64 maximum_version, const QString& platform)
    -> AiModelPackageComparison;

/// Action offered for @p status; none when the feed offers no supported entry.
[[nodiscard]] auto AiModelPackageActionFor(AiModelPackageStatus status, bool offered)
    -> AiModelPackageAction;

/// Whether the AI Mask tool can load the installed AI Mask model set (AI Models plan, AM5).
enum class AiMaskModelSetAvailability : std::uint8_t {
  /// A complete set of a supported version is installed.
  kReady,
  kNotInstalled,
  /// Only a version below this build's minimum is installed.
  kUpdateRequired,
  /// Only a version above this build's maximum is installed, or the checked feed lists only
  /// such versions.
  kNeedsNewerApplication,
  /// The installed set of a supported version does not agree with its inventory.
  kRepairRequired,
};

struct AiMaskModelSetReadiness {
  AiMaskModelSetAvailability availability = AiMaskModelSetAvailability::kNotInstalled;
  /// The installation directory of the complete supported set; empty unless kReady.
  std::filesystem::path      directory;
};

/// Readiness of the AI Mask tool from @p installation (the AI Mask range of this build) and the
/// comparison status @p compared with the last checked feed (kNeedsNewerApplication is known
/// only from the feed). Pure function; reads no file.
[[nodiscard]] auto AiMaskModelSetReadinessFor(const AiModelInstallation& installation,
                                              AiModelPackageStatus       compared)
    -> AiMaskModelSetReadiness;

/// Owner of the signed AI model package feed state, the local installations of the AI model
/// packages that this build knows (the AI Mask model set), and their download, install,
/// update, repair, and removal (docs/roadmap/alcedo_studio/ai/external_ai_model_download_plan.md,
/// AM1).
///
/// The feed is fetched only by CheckPackages() (Settings opening or an explicit retry), never at
/// startup. An archive is downloaded only by InstallPackage(), and only after the download
/// statement is accepted. The download uses the shared transfer admission (one job at a time);
/// verification, extraction, and activation run on one worker thread.
///
/// Thread affinity: all methods and signals on the owner (GUI) thread.
class AiModelPackageService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled CONSTANT)
  Q_PROPERTY(bool checking READ checking NOTIFY StateChanged)
  Q_PROPERTY(bool checked READ checked NOTIFY StateChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY StateChanged)
  Q_PROPERTY(QVariantList packages READ packages NOTIFY StateChanged)
  Q_PROPERTY(bool downloadStatementAccepted READ download_statement_accepted NOTIFY StateChanged)

 public:
  struct PackageState {
    QString                  id;
    quint64                  minimum_version = 0;
    quint64                  maximum_version = 0;
    AiModelInstallation      installation;
    AiModelPackageComparison comparison;
    /// Effective status: a transfer stage while an action runs, else the comparison.
    AiModelPackageStatus     status   = AiModelPackageStatus::kNotInstalled;
    double                   progress = 0.0;
    QString                  error;
  };

  AiModelPackageService(AiModelPackageServiceOptions options, QObject* parent = nullptr);
  ~AiModelPackageService() override;

  AiModelPackageService(const AiModelPackageService&)            = delete;
  AiModelPackageService& operator=(const AiModelPackageService&) = delete;

  [[nodiscard]] auto     enabled() const -> bool;
  [[nodiscard]] auto     checking() const -> bool { return checking_; }
  [[nodiscard]] auto     checked() const -> bool { return manifest_.has_value(); }
  [[nodiscard]] auto     last_error() const -> QString { return last_error_; }
  [[nodiscard]] auto     download_statement_accepted() const -> bool;
  /// One map per known package: `id`, `name`, `status` (AiModelPackageStatus name), `action`
  /// ("", "install", "update", "repair", "retry"), `available` (the feed offers a supported
  /// entry), `modelSetVersion` and `revision` (offered), `installedModelSetVersion` and
  /// `installedRevision` (0 and "" when none), `minimumSupportedVersion`,
  /// `maximumSupportedVersion`, `archiveBytes`, `licenses` (maps with `name` and `file`),
  /// `busy`, `cancelable`, `removable`, `progress` (0-1, download), and `error`.
  [[nodiscard]] auto     packages() const -> QVariantList;
  /// Scoped const read of one package; nullptr for an unknown ID.
  [[nodiscard]] auto     Package(const QString& package_id) const -> const PackageState*;

  /// Read the local installations again (for example after the model directory changed).
  /// Ignored while a package action runs.
  void                   RefreshInstallations();
  /// Read the local installations again, then the readiness of the AI Mask model set for the AI
  /// Mask tool (AM5). Reads file metadata only; makes no network request. While a package
  /// action runs, the installation read when the action started is used: the installed set
  /// changes only at the commit point, after which the service reads it again.
  [[nodiscard]] auto     ReadAiMaskModelSet() -> AiMaskModelSetReadiness;
  /// Recover an interrupted install or removal, then fetch and verify the signed feed and
  /// compare every known package. Never downloads an archive. Returns false when disabled,
  /// already checking, or while a package action runs.
  auto                   CheckPackages() -> bool;
  /// Record that the user accepted the current download statement.
  void                   AcceptDownloadStatement();
  /// Download, verify, and install the offered entry of @p package_id. Returns false when the
  /// statement is not accepted (DownloadStatementRequired is emitted), when nothing is offered,
  /// while another action runs, or when the shared transfer admission rejects the download
  /// (the package error is set).
  auto                   InstallPackage(const QString& package_id) -> bool;
  /// Cancel the download or the pre-commit installation of @p package_id.
  auto                   CancelInstall(const QString& package_id) -> bool;
  /// Remove the installed set of @p package_id. Works without a feed check.
  auto                   RemovePackage(const QString& package_id) -> bool;
  /// Cancel an active download, stop the worker, and ignore later completions. Idempotent.
  void                   Shutdown();

  Q_INVOKABLE bool       checkPackages() { return CheckPackages(); }
  Q_INVOKABLE void       refreshInstallations() { RefreshInstallations(); }
  Q_INVOKABLE void       acceptDownloadStatement() { AcceptDownloadStatement(); }
  Q_INVOKABLE bool       installPackage(const QString& id) { return InstallPackage(id); }
  Q_INVOKABLE bool       cancelInstall(const QString& id) { return CancelInstall(id); }
  Q_INVOKABLE bool       removePackage(const QString& id) { return RemovePackage(id); }

 signals:
  void StateChanged();
  void PackageChanged(const QString& package_id);
  /// An install was requested before the current download statement was accepted.
  void DownloadStatementRequired(const QString& package_id);
  /// The download of @p package_id started.
  void InstallStarted(const QString& package_id);
  /// The install of @p package_id ended: installed, canceled, or failed with @p message.
  void InstallFinished(const QString& package_id, bool installed, bool canceled,
                       const QString& message);

 private:
  void               FetchSmallFile(const QUrl& url, qint64 maximum_bytes,
                                    std::function<void(QByteArray, QString)> done);
  void               HandleFeed(QByteArray manifest_bytes, QByteArray signature_text);
  void               FailCheck(QString message);
  void               Recompare();
  void               StartInstallWorker(const QString& package_id);
  void               FinishInstall(const QString& package_id, AiModelPackageInstallOutcome outcome);
  void               FinishRemoval(const QString& package_id, QString error);
  void               SetPackageStatus(const QString& package_id, AiModelPackageStatus status);
  void               JoinWorker();
  [[nodiscard]] auto MutablePackage(const QString& package_id) -> PackageState*;
  [[nodiscard]] auto ArchivePath(const PackageState& package) const -> std::filesystem::path;
  [[nodiscard]] auto ActionRunning() const -> bool { return !active_package_id_.isEmpty(); }

  AiModelPackageServiceOptions                options_;
  QNetworkAccessManager                       network_;
  std::vector<PackageState>                   packages_;
  /// The last verified feed; PackageState::comparison.offer points into it.
  std::optional<AiModelPackageManifest>       manifest_;
  QString                                     last_error_;
  /// The single package action (download, installation, or removal) that is running.
  QString                                     active_package_id_;
  QString                                     active_request_id_;
  /// Model directory and the request of the running installation, fixed when it starts.
  std::filesystem::path                       active_model_directory_;
  std::optional<AiModelPackageInstallRequest> active_request_;
  bool                                        installing_ = false;
  bool                                        removing_   = false;
  bool                                        checking_   = false;
  bool                                        shut_down_  = false;
  std::atomic<bool>                           stop_{false};
  std::thread                                 worker_;
};

}  // namespace alcedo
