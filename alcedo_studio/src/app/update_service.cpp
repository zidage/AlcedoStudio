//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/update_service.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

#ifndef ALCEDO_APP_VERSION
#define ALCEDO_APP_VERSION "0.0.0"
#endif
#ifndef ALCEDO_BUILD_NUMBER
#define ALCEDO_BUILD_NUMBER 0
#endif
#ifndef ALCEDO_UPDATE_MANIFEST_URL
#define ALCEDO_UPDATE_MANIFEST_URL ""
#endif
#ifndef ALCEDO_UPDATE_PUBLIC_KEY_BASE64
#define ALCEDO_UPDATE_PUBLIC_KEY_BASE64 ""
#endif

namespace alcedo {
namespace {

constexpr qint64 kMaximumManifestBytes  = 256 * 1024;
constexpr qint64 kMaximumSignatureBytes = 1024;
constexpr auto   kTrustedSequenceKey    = "updates/highestTrustedSequence";

auto             NewRequest(const QUrl& url) -> QNetworkRequest {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                   QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(15000);
  request.setRawHeader("Accept", "application/json, text/plain;q=0.9");
  request.setRawHeader("Cache-Control", "no-cache");
  return request;
}

auto SafePackageName(const QUrl& url) -> QString {
  const QString suffix = QFileInfo(url.path()).suffix().toLower();
  if (suffix == QStringLiteral("exe") || suffix == QStringLiteral("zip")) {
    return QStringLiteral("package.") + suffix;
  }
  return QStringLiteral("package.bin");
}

}  // namespace

UpdateService::UpdateService(QObject* parent)
    : QObject(parent),
      public_key_(QByteArray::fromBase64(QByteArrayLiteral(ALCEDO_UPDATE_PUBLIC_KEY_BASE64),
                                         QByteArray::AbortOnBase64DecodingErrors)),
      feed_url_(QStringLiteral(ALCEDO_UPDATE_MANIFEST_URL)) {
  if (public_key_.size() == 32 && feed_url_.isValid() &&
      feed_url_.scheme() == QStringLiteral("https") && feed_url_.userInfo().isEmpty()) {
    state_       = State::Idle;
    status_text_ = tr("Updates are ready to check.");
  } else {
    public_key_.clear();
    state_       = State::Disabled;
    status_text_ = tr("Updates are disabled in this build.");
  }
}

UpdateService::~UpdateService() { ResetPackageDownload(); }

QString UpdateService::current_version() const { return QStringLiteral(ALCEDO_APP_VERSION); }

QString UpdateService::available_version() const {
  return manifest_.has_value() ? manifest_->version : QString{};
}

bool UpdateService::update_available() const {
  return state_ == State::Available || state_ == State::Downloading || state_ == State::Ready;
}

bool UpdateService::busy() const {
  return state_ == State::Checking || state_ == State::Downloading || state_ == State::Installing;
}

QUrl UpdateService::notes_url() const {
  return manifest_.has_value() ? manifest_->notes_url : QUrl{};
}

void UpdateService::SetState(State state, QString status, QString error) {
  state_       = state;
  status_text_ = std::move(status);
  error_text_  = std::move(error);
  emit changed();
}

void UpdateService::Fail(QString message) {
  staged_helper_.clear();
  installer_arguments_.clear();
  ResetPackageDownload();
  SetState(State::Error, tr("The update operation failed."), std::move(message));
}

void UpdateService::FetchSmallFile(const QUrl& url, qint64 maximum_size, SmallReply callback) {
  if (url.scheme() != QStringLiteral("https") ||
      url.host().compare(feed_url_.host(), Qt::CaseInsensitive) != 0 || !url.userInfo().isEmpty()) {
    callback({}, tr("The update server URL is not allowed."));
    return;
  }

  QNetworkReply* reply = network_.get(NewRequest(url));
  active_reply_        = reply;
  auto payload         = std::make_shared<QByteArray>();
  connect(reply, &QNetworkReply::readyRead, this, [reply, payload, maximum_size]() {
    payload->append(reply->readAll());
    if (payload->size() > maximum_size) {
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, payload, maximum_size, callback = std::move(callback)]() mutable {
            if (active_reply_ == reply) {
              active_reply_.clear();
            }
            payload->append(reply->readAll());
            QString   error;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
              error = reply->errorString();
            } else if (status != 200) {
              error = tr("The update server returned HTTP %1.").arg(status);
            } else if (reply->url().scheme() != QStringLiteral("https") ||
                       reply->url().host().compare(feed_url_.host(), Qt::CaseInsensitive) != 0) {
              error = tr("The update server redirected to an untrusted host.");
            } else if (payload->size() > maximum_size) {
              error = tr("The update server response is too large.");
            }
            reply->deleteLater();
            callback(error.isEmpty() ? std::move(*payload) : QByteArray{}, std::move(error));
          });
}

void UpdateService::CheckForUpdates() {
  if (!enabled() || busy()) {
    return;
  }
  manifest_.reset();
  package_path_.clear();
  progress_ = 0.0;
  SetState(State::Checking, tr("Checking for updates…"));

  FetchSmallFile(
      feed_url_, kMaximumManifestBytes, [this](QByteArray manifest_bytes, QString error) {
        if (!error.isEmpty()) {
          Fail(std::move(error));
          return;
        }
        QUrl signature_url = feed_url_;
        signature_url.setPath(signature_url.path() + QStringLiteral(".sig"));
        FetchSmallFile(signature_url, kMaximumSignatureBytes,
                       [this, manifest_bytes = std::move(manifest_bytes)](
                           QByteArray signature_text, QString signature_error) mutable {
                         if (!signature_error.isEmpty()) {
                           Fail(std::move(signature_error));
                           return;
                         }
                         HandleManifest(std::move(manifest_bytes), std::move(signature_text));
                       });
      });
}

void UpdateService::HandleManifest(QByteArray manifest_bytes, QByteArray signature_text) {
  const quint64 trusted_sequence =
      QSettings{}.value(QLatin1String(kTrustedSequenceKey), 1).toULongLong();
  UpdateManifestResult result = VerifyUpdateManifest(manifest_bytes, signature_text, public_key_,
                                                     PlatformKey(), feed_url_, trusted_sequence);
  if (!result) {
    Fail(std::move(result.error));
    return;
  }

  manifest_ = std::move(*result.manifest);
  QSettings{}.setValue(QLatin1String(kTrustedSequenceKey), manifest_->sequence);
  if (manifest_->build <= static_cast<quint64>(ALCEDO_BUILD_NUMBER)) {
    SetState(State::UpToDate, tr("Alcedo Studio is up to date."));
    return;
  }
  SetState(State::Available, tr("Alcedo Studio %1 is available.").arg(manifest_->version));
}

void UpdateService::DownloadUpdate() {
  if (state_ != State::Available || !manifest_.has_value()) {
    return;
  }

  const QString update_dir =
      QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
          .filePath(QStringLiteral("updates/%1").arg(manifest_->version));
  if (!QDir{}.mkpath(update_dir)) {
    Fail(tr("The update download directory cannot be created."));
    return;
  }
  package_path_              = QDir(update_dir).filePath(SafePackageName(manifest_->artifact.url));
  const QString partial_path = package_path_ + QStringLiteral(".part");
  QFile::remove(partial_path);
  download_file_ = std::make_unique<QFile>(partial_path);
  if (!download_file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    Fail(download_file_->errorString());
    return;
  }
  download_hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  progress_      = 0.0;
  SetState(State::Downloading, tr("Downloading Alcedo Studio %1…").arg(manifest_->version));

  QNetworkReply* reply = network_.get(NewRequest(manifest_->artifact.url));
  active_reply_        = reply;
  connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
    if (!download_file_ || !download_hash_ || !manifest_.has_value()) {
      return;
    }
    const QByteArray data = reply->readAll();
    if (download_file_->size() + data.size() > manifest_->artifact.size ||
        download_file_->write(data) != data.size()) {
      reply->abort();
      return;
    }
    download_hash_->addData(data);
  });
  connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
    if (!manifest_.has_value()) {
      return;
    }
    const qint64 expected = total > 0 ? total : manifest_->artifact.size;
    progress_ = expected > 0 ? std::clamp(static_cast<double>(received) / expected, 0.0, 1.0) : 0.0;
    emit changed();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (active_reply_ == reply) {
      active_reply_.clear();
    }
    if (download_file_ && reply->bytesAvailable() > 0) {
      const QByteArray data = reply->readAll();
      if (manifest_.has_value() &&
          download_file_->size() + data.size() <= manifest_->artifact.size &&
          download_file_->write(data) == data.size()) {
        download_hash_->addData(data);
      }
    }
    QString   error;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
      error = reply->errorString();
    } else if (status != 200) {
      error = tr("The update server returned HTTP %1.").arg(status);
    } else if (reply->url().scheme() != QStringLiteral("https") ||
               reply->url().host().compare(feed_url_.host(), Qt::CaseInsensitive) != 0) {
      error = tr("The update package redirected to an untrusted host.");
    }
    reply->deleteLater();
    if (!error.isEmpty()) {
      Fail(error);
      return;
    }
    FinishPackageDownload();
  });
}

void UpdateService::FinishPackageDownload() {
  if (!download_file_ || !download_hash_ || !manifest_.has_value()) {
    Fail(tr("The update download state is not valid."));
    return;
  }
  const QString partial_path = download_file_->fileName();
  download_file_->flush();
  download_file_->close();
  const qint64     size   = QFileInfo(partial_path).size();
  const QByteArray digest = download_hash_->result();
  download_file_.reset();
  download_hash_.reset();

  if (size != manifest_->artifact.size || digest != manifest_->artifact.sha256) {
    QFile::remove(partial_path);
    Fail(tr("The downloaded package does not match the signed manifest."));
    return;
  }
  QFile::remove(package_path_);
  if (!QFile::rename(partial_path, package_path_)) {
    QFile::remove(partial_path);
    Fail(tr("The verified update package cannot be saved."));
    return;
  }
  progress_ = 1.0;
  SetState(State::Ready, tr("Alcedo Studio %1 is ready to install.").arg(manifest_->version));
}

void UpdateService::ResetPackageDownload() {
  if (active_reply_) {
    active_reply_->abort();
    active_reply_.clear();
  }
  if (download_file_) {
    const QString path = download_file_->fileName();
    download_file_->close();
    download_file_.reset();
    QFile::remove(path);
  }
  download_hash_.reset();
}

bool UpdateService::PackageMatchesManifest(QString* error) const {
  if (!manifest_.has_value() || package_path_.isEmpty()) {
    *error = tr("No verified update package is available.");
    return false;
  }
  QFile file(package_path_);
  if (!file.open(QIODevice::ReadOnly) || file.size() != manifest_->artifact.size) {
    *error = tr("The update package is missing or has changed.");
    return false;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file) || hash.result() != manifest_->artifact.sha256) {
    *error = tr("The update package failed its final SHA-256 check.");
    return false;
  }
  return true;
}

void UpdateService::InstallUpdate() {
  if (state_ != State::Ready) {
    return;
  }
  QString error;
  if (!PackageMatchesManifest(&error)) {
    Fail(std::move(error));
    return;
  }

#ifdef Q_OS_MACOS
  const QFileInfo current_bundle(CurrentInstallPath());
  if (!current_bundle.dir().isWritable()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(package_path_));
    Fail(
        tr("Alcedo Studio cannot replace the app in this folder. The update ZIP was opened for "
           "manual installation."));
    return;
  }
#endif

  const QString installed_helper = InstallerHelperPath();
#ifdef Q_OS_WIN
  const QString staged_helper =
      QFileInfo(package_path_).dir().filePath(QStringLiteral("alcedo_update_installer.exe"));
#else
  const QString staged_helper = installed_helper;
#endif
#ifdef Q_OS_WIN
  QFile::remove(staged_helper);
  if (!QFile::copy(installed_helper, staged_helper)) {
    Fail(tr("The update installer helper is missing."));
    return;
  }
  QFile::setPermissions(staged_helper, QFile::permissions(staged_helper) | QFileDevice::ExeOwner |
                                           QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  const QDir application_dir(QCoreApplication::applicationDirPath());
  const QDir staging_dir    = QFileInfo(staged_helper).dir();
  bool       copied_qt_core = false;
  for (const QString& dll_name : {QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Cored.dll")}) {
    const QString source = application_dir.filePath(dll_name);
    if (!QFileInfo::exists(source)) {
      continue;
    }
    const QString destination = staging_dir.filePath(dll_name);
    QFile::remove(destination);
    copied_qt_core = QFile::copy(source, destination) || copied_qt_core;
  }
  if (!copied_qt_core) {
    Fail(tr("The Qt runtime for the update installer helper is missing."));
    return;
  }
#else
  if (!QFileInfo::exists(staged_helper)) {
    Fail(tr("The update installer helper is missing."));
    return;
  }
#endif

  staged_helper_       = staged_helper;
  installer_arguments_ = {
      QStringLiteral("--wait-pid"),       QString::number(QCoreApplication::applicationPid()),
      QStringLiteral("--package"),        package_path_,
      QStringLiteral("--sha256"),         QString::fromLatin1(manifest_->artifact.sha256.toHex()),
      QStringLiteral("--install-path"),   CurrentInstallPath(),
      QStringLiteral("--app-executable"), QCoreApplication::applicationFilePath()};

  SetState(State::Installing, tr("Alcedo Studio will close and install the update."));
  emit applicationCloseRequested();
}

bool UpdateService::CommitInstall() {
  if (state_ != State::Installing || staged_helper_.isEmpty() || installer_arguments_.isEmpty()) {
    return false;
  }

  qint64 helper_pid = 0;
  if (!QProcess::startDetached(staged_helper_, installer_arguments_, {}, &helper_pid) ||
      helper_pid <= 0) {
    Fail(tr("The update installer helper could not start."));
    return false;
  }
  staged_helper_.clear();
  installer_arguments_.clear();
  return true;
}

void UpdateService::CancelInstall() {
  if (state_ != State::Installing) {
    return;
  }
  staged_helper_.clear();
  installer_arguments_.clear();
  SetState(State::Ready, tr("Alcedo Studio %1 is ready to install.").arg(manifest_->version));
}

void UpdateService::Dismiss() {
  if (!busy()) {
    SetState(enabled() ? State::Idle : State::Disabled,
             enabled() ? tr("Update hidden for this session.")
                       : tr("Updates are disabled in this build."));
  }
}

void UpdateService::OpenReleaseNotes() {
  if (manifest_.has_value() && manifest_->notes_url.isValid()) {
    QDesktopServices::openUrl(manifest_->notes_url);
  }
}

QString UpdateService::PlatformKey() const {
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
  return QStringLiteral("windows-x86_64");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
  return QStringLiteral("macos-arm64");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
  return QStringLiteral("macos-x86_64");
#else
  return QStringLiteral("unsupported");
#endif
}

QString UpdateService::InstallerHelperPath() const {
#ifdef Q_OS_MACOS
  QDir executable_dir(QCoreApplication::applicationDirPath());
  executable_dir.cdUp();
  return executable_dir.filePath(QStringLiteral("Helpers/alcedo_update_installer"));
#else
  return QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("alcedo_update_installer.exe"));
#endif
}

QString UpdateService::CurrentInstallPath() const {
#ifdef Q_OS_MACOS
  QDir bundle(QCoreApplication::applicationDirPath());
  bundle.cdUp();
  bundle.cdUp();
  return bundle.absolutePath();
#else
  QDir install(QCoreApplication::applicationDirPath());
  install.cdUp();
  return install.absolutePath();
#endif
}

}  // namespace alcedo
