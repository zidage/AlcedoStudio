//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
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

class QNetworkReply;

namespace alcedo {

/// Application service for signed, user-confirmed application updates.
class UpdateService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(State state READ state NOTIFY changed)
  Q_PROPERTY(bool enabled READ enabled CONSTANT)
  Q_PROPERTY(QString currentVersion READ current_version CONSTANT)
  Q_PROPERTY(QString availableVersion READ available_version NOTIFY changed)
  Q_PROPERTY(QString statusText READ status_text NOTIFY changed)
  Q_PROPERTY(QString errorText READ error_text NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(bool updateAvailable READ update_available NOTIFY changed)
  Q_PROPERTY(bool downloadReady READ download_ready NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QUrl notesUrl READ notes_url NOTIFY changed)

 public:
  enum class State {
    Disabled,
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Ready,
    Installing,
    Error
  };
  Q_ENUM(State)

  explicit UpdateService(QObject* parent = nullptr);
  ~UpdateService() override;

  [[nodiscard]] State   state() const { return state_; }
  [[nodiscard]] bool    enabled() const { return public_key_.size() == 32; }
  [[nodiscard]] QString current_version() const;
  [[nodiscard]] QString available_version() const;
  [[nodiscard]] QString status_text() const { return status_text_; }
  [[nodiscard]] QString error_text() const { return error_text_; }
  [[nodiscard]] double  progress() const { return progress_; }
  [[nodiscard]] bool    update_available() const;
  [[nodiscard]] bool    download_ready() const { return state_ == State::Ready; }
  [[nodiscard]] bool    busy() const;
  [[nodiscard]] QUrl    notes_url() const;

  Q_INVOKABLE void      CheckForUpdates();
  Q_INVOKABLE void      DownloadUpdate();
  Q_INVOKABLE void      InstallUpdate();
  Q_INVOKABLE bool      CommitInstall();
  Q_INVOKABLE void      CancelInstall();
  Q_INVOKABLE void      Dismiss();
  Q_INVOKABLE void      OpenReleaseNotes();

 signals:
  void changed();
  void applicationCloseRequested();

 private:
  using SmallReply = std::function<void(QByteArray, QString)>;

  void                    SetState(State state, QString status = {}, QString error = {});
  void                    FetchSmallFile(const QUrl& url, qint64 maximum_size, SmallReply callback);
  void                    HandleManifest(QByteArray manifest_bytes, QByteArray signature_text);
  void                    Fail(QString message);
  void                    FinishPackageDownload();
  void                    ResetPackageDownload();
  [[nodiscard]] bool      PackageMatchesManifest(QString* error) const;
  [[nodiscard]] QString   PlatformKey() const;
  [[nodiscard]] QString   InstallerHelperPath() const;
  [[nodiscard]] QString   CurrentInstallPath() const;

  QNetworkAccessManager   network_;
  QPointer<QNetworkReply> active_reply_;
  std::unique_ptr<QFile>  download_file_;
  std::unique_ptr<QCryptographicHash> download_hash_;
  std::optional<UpdateManifest>       manifest_;
  QByteArray                          public_key_;
  QUrl                                feed_url_;
  QString                             package_path_;
  QString                             staged_helper_;
  QStringList                         installer_arguments_;
  QString                             status_text_;
  QString                             error_text_;
  State                               state_    = State::Disabled;
  double                              progress_ = 0.0;
};

}  // namespace alcedo
