//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QString>

#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// JSON-RPC 2.0 server on a `QLocalServer`, one JSON object per line.
///
/// Accepts several connections. Each connection keeps its own read buffer and receives the
/// responses to its own requests. Notifications go to all connections. The server and every
/// command handler run on the thread that owns the server (the GUI thread).
class AutomationServer final : public QObject {
  Q_OBJECT

 public:
  /// @p registry must outlive the server.
  explicit AutomationServer(const AutomationCommandRegistry& registry, QObject* parent = nullptr);
  ~AutomationServer() override;

  AutomationServer(const AutomationServer&)                                  = delete;
  auto               operator=(const AutomationServer&) -> AutomationServer& = delete;

  /// Listens on @p socket_name with `QLocalServer::UserAccessOption`. Returns false and writes
  /// the Qt error string to @p error when the name is in use or the listen fails.
  [[nodiscard]] auto Listen(const QString& socket_name, QString* error = nullptr) -> bool;

  /// Stops listening and closes every connection.
  void               Close();

  [[nodiscard]] auto IsListening() const -> bool;
  /// The name passed to `Listen`.
  [[nodiscard]] auto socket_name() const -> QString;
  /// The platform path of the socket (a named pipe on Windows).
  [[nodiscard]] auto full_server_name() const -> QString;
  [[nodiscard]] auto connection_count() const -> int;

  /// Sends one notification line to every open connection.
  void               SendNotification(const QString& method, const QJsonObject& params);

 private:
  void        HandleNewConnection();
  void        HandleReadyRead(QLocalSocket* socket);
  void        HandleDisconnected(QLocalSocket* socket);
  void        HandleLine(QLocalSocket* socket, const QByteArray& line);
  void        RejectOversizedLine(QLocalSocket* socket);

  static void WriteMessage(QLocalSocket* socket, const AutomationMessage& message);

  const AutomationCommandRegistry& registry_;
  QLocalServer                     server_;
  QHash<QLocalSocket*, QByteArray> read_buffers_;
};

}  // namespace alcedo::automation
