//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

// A stand-in for aria2c that answers the JSON-RPC calls of DownloadService over HTTP on
// --rpc-listen-port. aria2.addUri copies the file at <ALCEDO_FAKE_ARIA2_SOURCE_DIR>/<URL path>
// to <dir>/<out> and reports the download complete. With ALCEDO_FAKE_ARIA2_HOLD=1 a download
// stays active without bytes until aria2.remove, so a test can cancel it.

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <utility>

namespace {

struct FakeDownload {
  QString state;
  qint64  total_length = 0;
  QString error_message;
};

class FakeAria2Rpc final : public QObject {
 public:
  FakeAria2Rpc(QString source_dir, bool hold) : source_dir_(std::move(source_dir)), hold_(hold) {}

  auto Listen(quint16 port) -> bool {
    connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket* socket = server_.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { Read(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    return server_.listen(QHostAddress::LocalHost, port);
  }

 private:
  void Read(QTcpSocket* socket) {
    QByteArray& buffer = buffers_[socket];
    buffer.append(socket->readAll());
    const qsizetype header_end = buffer.indexOf("\r\n\r\n");
    if (header_end < 0) {
      return;
    }
    qint64 content_length = 0;
    for (const QByteArray& line : buffer.left(header_end).split('\n')) {
      const QByteArray trimmed = line.trimmed();
      if (trimmed.toLower().startsWith("content-length:")) {
        content_length = trimmed.mid(15).trimmed().toLongLong();
      }
    }
    if (buffer.size() < header_end + 4 + content_length) {
      return;
    }
    const QByteArray body     = buffer.mid(header_end + 4, content_length);
    bool             shutdown = false;
    const QByteArray reply    = Handle(QJsonDocument::fromJson(body).object(), &shutdown);
    buffers_.remove(socket);
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                  QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply);
    socket->flush();
    socket->disconnectFromHost();
    if (shutdown) {
      QTimer::singleShot(50, qApp, &QCoreApplication::quit);
    }
  }

  auto Handle(const QJsonObject& request, bool* shutdown) -> QByteArray {
    const QString    method = request.value(QStringLiteral("method")).toString();
    const QJsonArray params = request.value(QStringLiteral("params")).toArray();
    QJsonValue       result;
    if (method == QStringLiteral("aria2.getVersion")) {
      result = QJsonObject{{QStringLiteral("version"), QStringLiteral("fake")}};
    } else if (method == QStringLiteral("aria2.addUri")) {
      result = AddUri(params.at(1).toArray().at(0).toString(), params.at(2).toObject());
    } else if (method == QStringLiteral("aria2.tellStatus")) {
      const FakeDownload download = downloads_.value(params.at(1).toString());
      const QString      done     = download.state == QStringLiteral("complete")
                                        ? QString::number(download.total_length)
                                        : QStringLiteral("0");
      result                      = QJsonObject{{QStringLiteral("status"), download.state},
                                                {QStringLiteral("totalLength"), QString::number(download.total_length)},
                                                {QStringLiteral("completedLength"), done},
                                                {QStringLiteral("downloadSpeed"), QStringLiteral("0")},
                                                {QStringLiteral("errorCode"), QStringLiteral("0")},
                                                {QStringLiteral("errorMessage"), download.error_message}};
    } else if (method == QStringLiteral("aria2.remove")) {
      downloads_[params.at(1).toString()].state = QStringLiteral("removed");
      result                                    = params.at(1);
    } else if (method == QStringLiteral("aria2.forceShutdown")) {
      *shutdown = true;
      result    = QStringLiteral("OK");
    } else {
      return QJsonDocument(QJsonObject{{QStringLiteral("id"), request.value(QStringLiteral("id"))},
                                       {QStringLiteral("error"),
                                        QJsonObject{{QStringLiteral("message"),
                                                     QStringLiteral("unknown method")}}}})
          .toJson(QJsonDocument::Compact);
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                     {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                                     {QStringLiteral("result"), result}})
        .toJson(QJsonDocument::Compact);
  }

  auto AddUri(const QString& uri, const QJsonObject& options) -> QString {
    const QString gid = QString::number(++next_gid_);
    FakeDownload  download;
    if (hold_) {
      download.state = QStringLiteral("active");
      downloads_.insert(gid, download);
      return gid;
    }
    const QString source      = QDir(source_dir_).filePath(QUrl(uri).path().mid(1));
    const QString destination = QDir(options.value(QStringLiteral("dir")).toString())
                                    .filePath(options.value(QStringLiteral("out")).toString());
    QFile::remove(destination);
    if (!QFileInfo::exists(source) || !QFile::copy(source, destination)) {
      download.state         = QStringLiteral("error");
      download.error_message = QStringLiteral("no source file for %1").arg(uri);
    } else {
      download.state        = QStringLiteral("complete");
      download.total_length = QFileInfo(destination).size();
    }
    downloads_.insert(gid, download);
    return gid;
  }

  QTcpServer                     server_;
  QHash<QTcpSocket*, QByteArray> buffers_;
  QHash<QString, FakeDownload>   downloads_;
  QString                        source_dir_;
  bool                           hold_     = false;
  int                            next_gid_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  quint16          port = 0;
  const auto       args = application.arguments();
  for (qsizetype i = 1; i + 1 < args.size(); ++i) {
    if (args.at(i) == QStringLiteral("--rpc-listen-port")) {
      port = static_cast<quint16>(args.at(i + 1).toUInt());
    }
  }
  FakeAria2Rpc rpc(qEnvironmentVariable("ALCEDO_FAKE_ARIA2_SOURCE_DIR"),
                   qEnvironmentVariable("ALCEDO_FAKE_ARIA2_HOLD") == QStringLiteral("1"));
  if (port == 0 || !rpc.Listen(port)) {
    return 2;
  }
  return application.exec();
}
