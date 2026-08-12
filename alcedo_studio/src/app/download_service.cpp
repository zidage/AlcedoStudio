//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/download_service.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace alcedo {
namespace {

constexpr int         kAria2ReadyTimeoutMs = 10000;
constexpr int         kAria2RpcTimeoutMs   = 15000;
constexpr int         kProgressPollMs      = 250;
constexpr int         kStopTimeoutMs       = 2000;
constexpr int         kConnections         = 16;
constexpr int         kStallTimeoutMs      = 120000;
constexpr const char* kAria2cBinaryEnv     = "ALCEDO_ARIA2C_BINARY";
#ifdef _WIN32
constexpr const char* kAria2cBinaryName = "aria2c.exe";
#else
constexpr const char* kAria2cBinaryName = "aria2c";
#endif

auto DefaultAria2cBinary() -> QString {
  const QByteArray configured = qgetenv(kAria2cBinaryEnv);
  return configured.isEmpty()
             ? QDir(QCoreApplication::applicationDirPath()).filePath(
                   QString::fromLatin1(kAria2cBinaryName))
             : QString::fromUtf8(configured);
}

auto ChooseFreePort() -> quint16 {
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, 0)) {
    return 6800;
  }
  const auto port = static_cast<quint16>(server.serverPort());
  server.close();
  return port;
}

auto GenerateSecret() -> QString {
  QByteArray bytes;
  for (int i = 0; i < 16; ++i) {
    bytes.append(static_cast<char>(QRandomGenerator::global()->bounded(256)));
  }
  return QString::fromLatin1(bytes.toHex());
}

auto FileMatches(const DownloadItem& item) -> bool {
  QFile file(item.destination);
  if (!file.open(QIODevice::ReadOnly)) {
    return false;
  }
  if (item.expected_size > 0 && file.size() != item.expected_size) {
    return false;
  }
  if (item.expected_sha256.isEmpty()) {
    return true;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) && hash.result() == item.expected_sha256;
}

class Aria2Rpc final : public QObject {
 public:
  explicit Aria2Rpc(QObject* parent = nullptr) : QObject(parent), network_(new QNetworkAccessManager(this)) {}

  auto Ping(const QUrl& url, const QString& secret) -> bool {
    url_    = url;
    secret_ = QStringLiteral("token:") + secret;
    return Call(QStringLiteral("aria2.getVersion"), QJsonArray{secret_}, 2000)
        .contains(QStringLiteral("result"));
  }

  auto Call(const QString& method, const QJsonArray& params, int timeout_ms) -> QJsonObject {
    QJsonObject body{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                     {QStringLiteral("id"), ++next_id_},
                     {QStringLiteral("method"), method},
                     {QStringLiteral("params"), params}};
    QNetworkRequest request(url_);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QEventLoop             loop;
    QNetworkReply*         reply = network_->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QPointer<QNetworkReply> guard(reply);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeout_ms, &loop, [guard] {
      if (guard) {
        guard->abort();
      }
    });
    loop.exec();
    QJsonObject response;
    if (guard && guard->error() == QNetworkReply::NoError) {
      const QJsonDocument document = QJsonDocument::fromJson(guard->readAll());
      if (document.isObject()) {
        response = document.object();
      }
    }
    if (guard) {
      guard->deleteLater();
    }
    return response;
  }

  [[nodiscard]] auto Secret() const -> QString { return secret_; }

 private:
  QNetworkAccessManager* network_ = nullptr;
  QUrl                   url_;
  QString                secret_;
  int                    next_id_ = 0;
};

void StopAria2(QProcess& process, Aria2Rpc* rpc) {
  if (rpc != nullptr) {
    rpc->Call(QStringLiteral("aria2.forceShutdown"),
              QJsonArray{rpc->Secret(), QStringLiteral("true")}, kStopTimeoutMs);
  }
  if (process.state() == QProcess::NotRunning) {
    return;
  }
  process.terminate();
  if (!process.waitForFinished(kStopTimeoutMs)) {
    process.kill();
    process.waitForFinished(kStopTimeoutMs);
  }
}

auto RpcError(const QJsonObject& response, const QString& file_name) -> QString {
  const QString message = response.value(QStringLiteral("error"))
                              .toObject()
                              .value(QStringLiteral("message"))
                              .toString();
  return QStringLiteral("aria2 RPC failed for %1: %2")
      .arg(file_name, message.isEmpty() ? QStringLiteral("no response from aria2c") : message);
}

}  // namespace

class DownloadWorker final : public QObject {
  Q_OBJECT

 public:
  explicit DownloadWorker(std::atomic<bool>& cancel_requested)
      : cancel_requested_(cancel_requested) {}

 signals:
  void ProgressChanged(const alcedo::DownloadProgress& progress);
  void Finished(const QString& id, bool ok, bool canceled, const QString& error);

 public slots:
  void Run(alcedo::DownloadRequest request) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    const QString binary = DefaultAria2cBinary();
    if (!QFileInfo::exists(binary)) {
      emit Finished(request.id, false, false,
                    QStringLiteral("aria2c was not found at %1").arg(binary));
      return;
    }

    const quint16 port   = ChooseFreePort();
    const QString secret = GenerateSecret();
    process.setProgram(binary);
    process.setArguments({QStringLiteral("--enable-rpc"), QStringLiteral("--rpc-listen-port"),
                          QString::number(port), QStringLiteral("--rpc-secret"), secret,
                          QStringLiteral("--rpc-allow-origin-all"), QStringLiteral("--quiet"),
                          QStringLiteral("--max-concurrent-downloads=1"),
                          QStringLiteral("--max-tries=5"), QStringLiteral("--retry-wait=2"),
                          QStringLiteral("--file-allocation=none"),
                          QStringLiteral("--connect-timeout=30"), QStringLiteral("--timeout=60"),
                          QStringLiteral("--lowest-speed-limit=10K")});
    process.start();
    if (!process.waitForStarted(kAria2ReadyTimeoutMs)) {
      emit Finished(request.id, false, false,
                    QStringLiteral("aria2c could not start: %1").arg(process.errorString()));
      return;
    }

    Aria2Rpc rpc;
    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/jsonrpc").arg(port));
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kAria2ReadyTimeoutMs);
    while (!rpc.Ping(endpoint, secret)) {
      if (cancel_requested_.load()) {
        StopAria2(process, nullptr);
        emit Finished(request.id, false, true, QStringLiteral("Download canceled by the user."));
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        StopAria2(process, nullptr);
        emit Finished(request.id, false, false,
                      QStringLiteral("aria2c RPC did not become ready."));
        return;
      }
      QThread::msleep(kProgressPollMs);
    }

    const QString error = RunItems(request, rpc);
    const bool canceled = cancel_requested_.load();
    StopAria2(process, &rpc);
    emit Finished(request.id, error.isEmpty() && !canceled, canceled,
                  canceled ? QStringLiteral("Download canceled by the user.") : error);
  }

 private:
  auto RunItems(const DownloadRequest& request, Aria2Rpc& rpc) -> QString {
    qint64 total_bytes = 0;
    for (const DownloadItem& item : request.items) {
      total_bytes += std::max<qint64>(0, item.expected_size);
    }
    qint64 completed_before = 0;
    int    files_completed  = 0;

    for (const DownloadItem& item : request.items) {
      if (cancel_requested_.load()) {
        return {};
      }
      if (FileMatches(item)) {
        completed_before += std::max<qint64>(0, item.expected_size);
        ++files_completed;
        emit ProgressChanged({request.id, QFileInfo(item.destination).fileName(),
                              completed_before, total_bytes, 0, files_completed,
                              static_cast<int>(request.items.size())});
        continue;
      }
      const QFileInfo destination(item.destination);
      if (!QDir{}.mkpath(destination.absolutePath())) {
        return QStringLiteral("The download directory cannot be created: %1")
            .arg(destination.absolutePath());
      }
      QJsonObject options{{QStringLiteral("split"), QString::number(kConnections)},
                          {QStringLiteral("max-connection-per-server"),
                           QString::number(kConnections)},
                          {QStringLiteral("continue"), QStringLiteral("true")},
                          {QStringLiteral("dir"), destination.absolutePath()},
                          {QStringLiteral("out"), destination.fileName()},
                          {QStringLiteral("min-split-size"), QStringLiteral("1M")},
                          {QStringLiteral("max-tries"), QStringLiteral("5")},
                          {QStringLiteral("retry-wait"), QStringLiteral("2")},
                          {QStringLiteral("file-allocation"), QStringLiteral("none")}};
      const QJsonObject add_response = rpc.Call(
          QStringLiteral("aria2.addUri"),
          QJsonArray{rpc.Secret(), QJsonArray{item.url.toString()}, options}, kAria2RpcTimeoutMs);
      const QString gid = add_response.value(QStringLiteral("result")).toString();
      if (gid.isEmpty()) {
        return RpcError(add_response, destination.fileName());
      }

      const QJsonArray keys{QStringLiteral("status"), QStringLiteral("totalLength"),
                            QStringLiteral("completedLength"), QStringLiteral("downloadSpeed"),
                            QStringLiteral("errorCode"), QStringLiteral("errorMessage")};
      qint64 last_completed = -1;
      auto   last_progress  = std::chrono::steady_clock::now();
      while (true) {
        if (cancel_requested_.load()) {
          rpc.Call(QStringLiteral("aria2.remove"), QJsonArray{rpc.Secret(), gid}, 2000);
          return {};
        }
        const QJsonObject response = rpc.Call(
            QStringLiteral("aria2.tellStatus"), QJsonArray{rpc.Secret(), gid, keys}, 3000);
        if (!response.contains(QStringLiteral("result"))) {
          return RpcError(response, destination.fileName());
        }
        const QJsonObject status = response.value(QStringLiteral("result")).toObject();
        const QString state      = status.value(QStringLiteral("status")).toString();
        const qint64 completed   = status.value(QStringLiteral("completedLength"))
                                      .toString()
                                      .toLongLong();
        const qint64 speed = status.value(QStringLiteral("downloadSpeed")).toString().toLongLong();
        if (completed != last_completed) {
          last_completed = completed;
          last_progress  = std::chrono::steady_clock::now();
        }
        emit ProgressChanged(
            {request.id, destination.fileName(),
             completed_before + std::clamp<qint64>(completed, 0, item.expected_size), total_bytes,
             std::max<qint64>(0, speed), files_completed,
             static_cast<int>(request.items.size())});

        if (state == QStringLiteral("complete")) {
          break;
        }
        if (state == QStringLiteral("error") || state == QStringLiteral("removed")) {
          const QString message = status.value(QStringLiteral("errorMessage")).toString();
          return QStringLiteral("aria2c failed to download %1: %2")
              .arg(destination.fileName(),
                   message.isEmpty() ? QStringLiteral("state %1").arg(state) : message);
        }
        if (std::chrono::steady_clock::now() - last_progress
            > std::chrono::milliseconds(kStallTimeoutMs)) {
          rpc.Call(QStringLiteral("aria2.remove"), QJsonArray{rpc.Secret(), gid}, 2000);
          return QStringLiteral("The download of %1 stalled for %2 seconds.")
              .arg(destination.fileName())
              .arg(kStallTimeoutMs / 1000);
        }
        QThread::msleep(kProgressPollMs);
      }

      if (!FileMatches(item)) {
        QFile::remove(item.destination);
        return QStringLiteral("The downloaded file failed its size or SHA-256 check: %1")
            .arg(destination.fileName());
      }
      completed_before += std::max<qint64>(0, item.expected_size);
      ++files_completed;
      emit ProgressChanged({request.id, destination.fileName(), completed_before, total_bytes, 0,
                            files_completed, static_cast<int>(request.items.size())});
    }
    return {};
  }

  std::atomic<bool>& cancel_requested_;
};

struct DownloadService::Impl {
  std::atomic<bool>    cancel_requested_{false};
  std::atomic<bool>    running_{false};
  QThread              worker_thread_;
  DownloadWorker*      worker_ = nullptr;
  QString              active_id_;

  Impl() {
    worker_ = new DownloadWorker(cancel_requested_);
    worker_->moveToThread(&worker_thread_);
    worker_thread_.start();
  }

  ~Impl() {
    cancel_requested_.store(true);
    worker_thread_.quit();
    worker_thread_.wait();
    delete worker_;
  }
};

DownloadService::DownloadService(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
  qRegisterMetaType<alcedo::DownloadRequest>("alcedo::DownloadRequest");
  qRegisterMetaType<alcedo::DownloadProgress>("alcedo::DownloadProgress");
  connect(impl_->worker_, &DownloadWorker::ProgressChanged, this, &DownloadService::ProgressChanged);
  connect(impl_->worker_, &DownloadWorker::Finished, this,
          [this](const QString& id, bool ok, bool canceled, const QString& error) {
            if (impl_->active_id_ != id) {
              return;
            }
            impl_->running_.store(false);
            impl_->active_id_.clear();
            emit Finished(id, ok, canceled, error);
          });
}

DownloadService::~DownloadService() = default;

auto DownloadService::Start(const DownloadRequest& request) -> bool {
  if (request.id.trimmed().isEmpty() || request.items.isEmpty() || impl_->running_.exchange(true)) {
    return false;
  }
  for (const DownloadItem& item : request.items) {
    const QString scheme = item.url.scheme().toLower();
    if (!item.url.isValid()
        || (scheme != QStringLiteral("https") && scheme != QStringLiteral("http"))
        || item.destination.trimmed().isEmpty() || item.expected_size <= 0) {
      impl_->running_.store(false);
      return false;
    }
  }
  impl_->cancel_requested_.store(false);
  impl_->active_id_ = request.id;
  QMetaObject::invokeMethod(impl_->worker_, [worker = impl_->worker_, request] { worker->Run(request); },
                            Qt::QueuedConnection);
  return true;
}

void DownloadService::Cancel(const QString& id) {
  if (impl_->running_.load() && impl_->active_id_ == id) {
    impl_->cancel_requested_.store(true);
  }
}

auto DownloadService::IsRunning() const -> bool { return impl_->running_.load(); }

auto DownloadService::ActiveRequestId() const -> QString { return impl_->active_id_; }

}  // namespace alcedo

#include "download_service.moc"
