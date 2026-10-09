//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_server.hpp"

#include <QDebug>
#include <QPointer>
#include <utility>
#include <variant>

namespace alcedo::automation {

AutomationServer::AutomationServer(const AutomationCommandRegistry& registry, QObject* parent)
    : QObject(parent), registry_(registry) {
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&server_, &QLocalServer::newConnection, this, &AutomationServer::HandleNewConnection);
}

AutomationServer::~AutomationServer() { Close(); }

auto AutomationServer::Listen(const QString& socket_name, QString* error) -> bool {
  if (server_.listen(socket_name)) {
    return true;
  }
  if (error != nullptr) {
    *error = QStringLiteral("cannot listen on '%1': %2").arg(socket_name, server_.errorString());
  }
  return false;
}

void AutomationServer::Close() {
  server_.close();
  const auto sockets = read_buffers_.keys();
  read_buffers_.clear();
  for (QLocalSocket* socket : sockets) {
    socket->disconnect(this);
    // Write the queued responses, for example the session.shutdown answer, before the close.
    socket->disconnectFromServer();
    if (socket->state() != QLocalSocket::UnconnectedState) {
      socket->waitForDisconnected(1000);
    }
    socket->deleteLater();
  }
}

auto AutomationServer::IsListening() const -> bool { return server_.isListening(); }

auto AutomationServer::socket_name() const -> QString { return server_.serverName(); }

auto AutomationServer::full_server_name() const -> QString { return server_.fullServerName(); }

auto AutomationServer::connection_count() const -> int {
  return static_cast<int>(read_buffers_.size());
}

void AutomationServer::SendNotification(const QString& method, const QJsonObject& params) {
  const QByteArray line = SerializeAutomationMessage(AutomationNotification{method, params});
  for (auto it = read_buffers_.cbegin(); it != read_buffers_.cend(); ++it) {
    QLocalSocket* socket = it.key();
    if (socket->state() == QLocalSocket::ConnectedState) {
      socket->write(line);
    }
  }
}

void AutomationServer::HandleNewConnection() {
  while (QLocalSocket* socket = server_.nextPendingConnection()) {
    read_buffers_.insert(socket, QByteArray());
    connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { HandleReadyRead(socket); });
    connect(socket, &QLocalSocket::disconnected, this,
            [this, socket]() { HandleDisconnected(socket); });
    // A peer can send and close before the connection is announced.
    if (socket->bytesAvailable() > 0) {
      HandleReadyRead(socket);
    }
  }
}

void AutomationServer::HandleReadyRead(QLocalSocket* socket) {
  const QPointer<QLocalSocket> guard(socket);
  auto                         buffer = read_buffers_.find(socket);
  if (buffer == read_buffers_.end()) {
    return;
  }
  buffer->append(socket->readAll());

  while (guard) {
    buffer = read_buffers_.find(socket);
    if (buffer == read_buffers_.end()) {
      return;
    }
    const qsizetype newline = buffer->indexOf('\n');
    if (newline < 0) {
      if (buffer->size() > kMaxAutomationLineBytes) {
        RejectOversizedLine(socket);
      }
      return;
    }
    if (newline > kMaxAutomationLineBytes) {
      RejectOversizedLine(socket);
      return;
    }
    const QByteArray line = buffer->left(newline);
    buffer->remove(0, newline + 1);
    if (line.trimmed().isEmpty()) {
      continue;
    }
    HandleLine(socket, line);
  }
}

void AutomationServer::HandleDisconnected(QLocalSocket* socket) {
  if (read_buffers_.remove(socket)) {
    socket->deleteLater();
  }
}

void AutomationServer::HandleLine(QLocalSocket* socket, const QByteArray& line) {
  AutomationParseResult parsed = ParseAutomationLine(line);
  if (parsed.error.has_value()) {
    WriteMessage(socket,
                 MakeAutomationErrorResponse(parsed.id, parsed.error->code, parsed.error->message));
    return;
  }

  auto* request = std::get_if<AutomationRequest>(&*parsed.message);
  if (request == nullptr) {
    WriteMessage(socket, MakeAutomationErrorResponse(
                             parsed.id, AutomationErrorCode::InvalidRequest,
                             QStringLiteral("the server accepts only requests with an id")));
    return;
  }

  AutomationReply reply(request->id,
                        [socket = QPointer<QLocalSocket>(socket)](AutomationResponse response) {
                          if (socket && socket->state() == QLocalSocket::ConnectedState) {
                            WriteMessage(socket, response);
                          }
                        });
  registry_.Dispatch(*request, std::move(reply));
}

void AutomationServer::RejectOversizedLine(QLocalSocket* socket) {
  qWarning().noquote() << "automation connection sent a line longer than" << kMaxAutomationLineBytes
                       << "bytes; closing it";
  // The connection reads no more input. It is deleted when the close completes.
  read_buffers_.remove(socket);
  socket->disconnect(this);
  connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
  WriteMessage(
      socket, MakeAutomationErrorResponse(
                  QJsonValue(QJsonValue::Null), AutomationErrorCode::ParseError,
                  QStringLiteral("the line is longer than %1 bytes").arg(kMaxAutomationLineBytes)));
  // Pending data is written before the connection closes.
  socket->disconnectFromServer();
}

void AutomationServer::WriteMessage(QLocalSocket* socket, const AutomationMessage& message) {
  socket->write(SerializeAutomationMessage(message));
}

}  // namespace alcedo::automation
