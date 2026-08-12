//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

#include "app/update_manifest.hpp"
#include "app/download_service.hpp"

class QNetworkReply;

namespace alcedo {

/// Application service for signed, user-confirmed application updates.
class UpdateService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(State state READ state NOTIFY changed)
  Q_PROPERTY(bool enabled READ enabled CONSTANT)
  Q_PROPERTY(bool installAllowed READ install_allowed CONSTANT)
  Q_PROPERTY(QString channel READ channel CONSTANT)
  Q_PROPERTY(QString currentVersion READ current_version CONSTANT)
  Q_PROPERTY(quint64 currentBuild READ current_build CONSTANT)
  Q_PROPERTY(QString availableVersion READ available_version NOTIFY changed)
  Q_PROPERTY(quint64 availableBuild READ available_build NOTIFY changed)
  Q_PROPERTY(QString changelog READ changelog NOTIFY changed)
  Q_PROPERTY(QString statusText READ status_text NOTIFY changed)
  Q_PROPERTY(QString errorText READ error_text NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QString downloadedBytesText READ downloaded_bytes_text NOTIFY changed)
  Q_PROPERTY(QString downloadSpeedText READ download_speed_text NOTIFY changed)
  Q_PROPERTY(QString downloadEtaText READ download_eta_text NOTIFY changed)
  Q_PROPERTY(bool unchecked READ unchecked NOTIFY changed)
  Q_PROPERTY(bool checking READ checking NOTIFY changed)
  Q_PROPERTY(bool offerAvailable READ offer_available NOTIFY changed)
  Q_PROPERTY(bool downloading READ downloading NOTIFY changed)
  Q_PROPERTY(bool installing READ installing NOTIFY changed)
  Q_PROPERTY(bool hasError READ has_error NOTIFY changed)
  Q_PROPERTY(bool updateAvailable READ update_available NOTIFY changed)
  Q_PROPERTY(bool updateDeferred READ update_deferred NOTIFY changed)
  Q_PROPERTY(bool downloadReady READ download_ready NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QUrl notesUrl READ notes_url NOTIFY changed)

 public:
  enum class State {
    Disabled,
    Unchecked,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Ready,
    Installing,
    Error
  };
  Q_ENUM(State)

  explicit UpdateService(DownloadService& downloads, QObject* parent = nullptr);
  ~UpdateService() override;

  [[nodiscard]] State   state() const { return state_; }
  [[nodiscard]] bool    enabled() const { return public_key_.size() == 32; }
  [[nodiscard]] bool    install_allowed() const;
  [[nodiscard]] QString channel() const;
  [[nodiscard]] QString current_version() const;
  [[nodiscard]] quint64 current_build() const;
  [[nodiscard]] QString available_version() const;
  [[nodiscard]] quint64 available_build() const;
  [[nodiscard]] QString changelog() const;
  [[nodiscard]] QString status_text() const { return status_text_; }
  [[nodiscard]] QString error_text() const { return error_text_; }
  [[nodiscard]] double  progress() const { return progress_; }
  [[nodiscard]] QString downloaded_bytes_text() const { return downloaded_bytes_text_; }
  [[nodiscard]] QString download_speed_text() const { return download_speed_text_; }
  [[nodiscard]] QString download_eta_text() const { return download_eta_text_; }
  [[nodiscard]] bool    unchecked() const { return state_ == State::Unchecked; }
  [[nodiscard]] bool    checking() const { return state_ == State::Checking; }
  [[nodiscard]] bool    offer_available() const { return state_ == State::Available; }
  [[nodiscard]] bool    downloading() const { return state_ == State::Downloading; }
  [[nodiscard]] bool    installing() const { return state_ == State::Installing; }
  [[nodiscard]] bool    has_error() const { return state_ == State::Error; }
  [[nodiscard]] bool    update_available() const;
  [[nodiscard]] bool    update_deferred() const { return deferred_; }
  [[nodiscard]] bool    download_ready() const { return state_ == State::Ready; }
  [[nodiscard]] bool    busy() const;
  [[nodiscard]] QUrl    notes_url() const;

  Q_INVOKABLE void CheckForUpdates();
  Q_INVOKABLE void DownloadUpdate();
  Q_INVOKABLE void CancelDownload();
  Q_INVOKABLE void InstallUpdate();
  Q_INVOKABLE bool CommitInstall();
  Q_INVOKABLE void CancelInstall();
  Q_INVOKABLE void DeferUpdate();
  Q_INVOKABLE void OpenReleaseNotes();

 signals:
  void changed();
  void offerReady();
  void applicationCloseRequested();

 private:
  using SmallReply = std::function<void(QByteArray, QString)>;

  void                  SetState(State state, QString status = {}, QString error = {});
  void                  FetchSmallFile(const QUrl& url, qint64 maximum_size, SmallReply callback);
  void                  HandleManifest(QByteArray manifest_bytes, QByteArray signature_text);
  void                  Fail(QString message);
  void                  FinishPackageDownload(const QString& downloaded_path);
  void                  ResetPackageDownload();
  [[nodiscard]] bool    PackageMatchesManifest(QString* error) const;
  [[nodiscard]] QString PlatformKey() const;
  [[nodiscard]] QString InstallerHelperPath() const;
  [[nodiscard]] QString CurrentInstallPath() const;

  QNetworkAccessManager               network_;
  QPointer<QNetworkReply>             active_reply_;
  DownloadService&                    downloads_;
  std::optional<UpdateManifest>       manifest_;
  QByteArray                          public_key_;
  QUrl                                feed_url_;
  QString                             package_path_;
  QString                             download_request_id_;
  QString                             staged_helper_;
  QStringList                         installer_arguments_;
  QString                             status_text_;
  QString                             error_text_;
  QString                             downloaded_bytes_text_;
  QString                             download_speed_text_;
  QString                             download_eta_text_;
  State                               state_     = State::Disabled;
  double                              progress_  = 0.0;
  bool                                deferred_  = false;
};

}  // namespace alcedo
