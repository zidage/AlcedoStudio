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
#include <QVector>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "app/download_service.hpp"
#include "app/lut_library_service.hpp"
#include "app/lut_package_manifest.hpp"

namespace alcedo {

/// Persistent LUT feed trust state, separate from the software-update sequence.
class LutPackagePreferences {
 public:
  virtual ~LutPackagePreferences()                                  = default;
  /// Highest feed sequence accepted so far; 1 before the first check.
  [[nodiscard]] virtual auto LoadTrustedSequence() const -> quint64 = 0;
  virtual void               SaveTrustedSequence(quint64 sequence)  = 0;
};

/// QSettings-backed preferences (`lut/packages/highestTrustedSequence`).
class SettingsLutPackagePreferences final : public LutPackagePreferences {
 public:
  [[nodiscard]] auto LoadTrustedSequence() const -> quint64 override;
  void               SaveTrustedSequence(quint64 sequence) override;
};

/// Archive transfer used by LUT package installation.
///
/// The production implementation routes every request through the shared
/// DownloadService, so a LUT package and an application or model download never
/// run two transfers at once: Start() returns false while another job runs.
class LutArchiveDownloader {
 public:
  using ProgressCallback = std::function<void(qint64 bytes_downloaded, qint64 bytes_total)>;
  using FinishedCallback = std::function<void(bool ok, bool canceled, const QString& error)>;

  virtual ~LutArchiveDownloader()                          = default;
  /// Start @p request; the callbacks run on the caller's thread. Returns false
  /// (and calls nothing) when the transport is busy or the request is invalid.
  virtual auto Start(const DownloadRequest& request, ProgressCallback on_progress,
                     FinishedCallback on_finished) -> bool = 0;
  virtual void Cancel(const QString& request_id)           = 0;
};

/// LutArchiveDownloader over the process-wide DownloadService. It must not
/// outlive @p downloads.
class DownloadServiceLutArchiveDownloader final : public QObject, public LutArchiveDownloader {
  Q_OBJECT

 public:
  explicit DownloadServiceLutArchiveDownloader(DownloadService& downloads,
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

/// Fetch at most @p maximum_bytes from @p url and call @p done with the bytes or
/// an error text. Called on the owner thread; @p done runs on the owner thread.
using LutFeedFetch = std::function<void(const QUrl& url, qint64 maximum_bytes,
                                        std::function<void(QByteArray bytes, QString error)> done)>;

struct LutPackageServiceOptions {
  /// Signed feed URL (`ALCEDO_LUT_PACKAGE_FEED_URL`). Empty disables packages.
  QUrl                                   feed_url;
  /// 32-byte Ed25519 public key (the software-update key).
  QByteArray                             public_key;
  /// Defaults to an HTTPS GET restricted to the feed host.
  LutFeedFetch                           fetch;
  /// Required.
  std::unique_ptr<LutArchiveDownloader>  downloader;
  /// Defaults to SettingsLutPackagePreferences.
  std::unique_ptr<LutPackagePreferences> preferences;

  /// Feed URL and key from this build's compile definitions.
  [[nodiscard]] static auto              FromBuildConfiguration() -> LutPackageServiceOptions;
};

/// Per-package state shown by Settings.
enum class LutPackageStatus {
  kNotInstalled,
  kCurrent,
  kUpdateAvailable,
  kRepairRequired,
  kChecking,
  kDownloading,
  kVerifying,
  kInstalling,
  kError
};

/// Comparison of one signed descriptor with the local installation.
struct LutPackageComparison {
  LutPackageStatus status           = LutPackageStatus::kNotInstalled;
  /// Installed LUT count and canonical digest computed from the published inventory.
  std::uint64_t    local_file_count = 0;
  std::string      local_inventory_sha256;
  /// False when the inventory is incomplete: an old summary is not proof of
  /// current on-disk integrity (plan 4.4).
  bool             local_verification_complete = false;
};

/// Compare @p descriptor with the installed package, using only the published
/// inventory of @p library (no file is read or hashed).
///
/// Not installed: no receipt. Update available: the receipt's verified digest
/// differs from the descriptor. Current: the installed entries produce the
/// descriptor's count and digest. Repair required: the receipt matches but the
/// installed entries do not (changed bytes, missing files, or a file whose
/// declared origin changed). Pure function of its inputs.
[[nodiscard]] auto      CompareLutPackage(const LutPackageDescriptor& descriptor,
                                          const LutLibraryService&    library) -> LutPackageComparison;

/// Owner of the signed LUT package feed state and the per-package check,
/// download, update, and repair actions
/// (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md L3).
///
/// Nothing runs at construction: the feed is fetched only by CheckPackages()
/// (Settings opening or an explicit retry), and an archive is downloaded only by
/// InstallPackage(). The download uses the shared transfer admission; extraction
/// and activation run as a LutLibraryService operation, so the library owner
/// serializes them with refresh, import, and root changes.
///
/// Thread affinity: all methods and signals on the owner (GUI) thread.
class LutPackageService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled CONSTANT)
  Q_PROPERTY(bool checking READ checking NOTIFY StateChanged)
  Q_PROPERTY(bool checked READ checked NOTIFY StateChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY StateChanged)
  Q_PROPERTY(QVariantList packages READ packages NOTIFY StateChanged)

 public:
  struct PackageState {
    LutPackageDescriptor descriptor;
    LutPackageComparison comparison;
    /// Effective status: a transfer stage while an action runs, else the comparison.
    LutPackageStatus     status   = LutPackageStatus::kNotInstalled;
    double               progress = 0.0;
    QString              error;
  };

  LutPackageService(LutPackageServiceOptions options, LutLibraryService& library,
                    QObject* parent = nullptr);
  ~LutPackageService() override;

  LutPackageService(const LutPackageService&)            = delete;
  LutPackageService& operator=(const LutPackageService&) = delete;

  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto checking() const -> bool { return checking_; }
  [[nodiscard]] auto checked() const -> bool { return !feed_manifest_.isEmpty(); }
  [[nodiscard]] auto last_error() const -> QString { return last_error_; }
  [[nodiscard]] auto packages() const -> QVariantList;
  /// Scoped const read of one package; nullptr when the feed does not list it.
  [[nodiscard]] auto Package(const QString& package_id) const -> const PackageState*;
  [[nodiscard]] auto Packages() const -> const std::vector<PackageState>& { return packages_; }

  /// Fetch and verify the signed feed, then compare every listed package with
  /// the local inventory. Never downloads an archive. Requests one inventory
  /// refresh when the local verification is incomplete. Returns false when
  /// disabled, already checking, or while a package action runs.
  auto               CheckPackages() -> bool;
  /// Download, verify, and activate @p package_id from the last verified feed.
  /// Returns false (with the package error set) when the package is unknown or
  /// busy, or when the shared transfer admission rejects the download.
  auto               InstallPackage(const QString& package_id) -> bool;
  /// Cancel a download or a pre-commit installation of @p package_id.
  auto               CancelInstall(const QString& package_id) -> bool;
  /// Cancel an active download and ignore later completions. Idempotent.
  void               Shutdown();

  Q_INVOKABLE bool   checkPackages() { return CheckPackages(); }
  Q_INVOKABLE bool   installPackage(const QString& id) { return InstallPackage(id); }
  Q_INVOKABLE bool   cancelInstall(const QString& id) { return CancelInstall(id); }

 signals:
  void StateChanged();
  void PackageChanged(const QString& package_id);

 private:
  void FetchSmallFile(const QUrl& url, qint64 maximum_bytes,
                      std::function<void(QByteArray, QString)> done);
  void HandleFeed(QByteArray manifest_bytes, QByteArray signature_text);
  void FailCheck(QString message);
  void RecomputeComparisons();
  void StartActivation(const QString& package_id);
  void OnLibraryOperationFinished(LutLibraryService::Operation operation,
                                  LutLibraryService::Status    status);
  void SetPackageStatus(const QString& package_id, LutPackageStatus status, QString error = {});
  [[nodiscard]] auto        MutablePackage(const QString& package_id) -> PackageState*;
  [[nodiscard]] auto        ArchivePath(const PackageState& package) const -> std::filesystem::path;
  [[nodiscard]] auto        ActionRunning() const -> bool { return !active_package_id_.isEmpty(); }

  LutPackageServiceOptions  options_;
  LutLibraryService&        library_;
  QNetworkAccessManager     network_;
  std::vector<PackageState> packages_;
  quint64                   feed_sequence_ = 0;
  /// Exact bytes of the last verified feed and its signature (install evidence).
  QByteArray                feed_manifest_;
  QByteArray                feed_signature_;
  QString                   last_error_;
  /// The single package action (download or activation) that is running.
  QString                   active_package_id_;
  QString                   active_request_id_;
  bool                      activation_pending_ = false;
  bool                      activating_         = false;
  bool                      checking_           = false;
  bool                      shut_down_          = false;
};

}  // namespace alcedo
