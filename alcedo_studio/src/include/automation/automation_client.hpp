//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QString>
#include <optional>

#include "automation/automation_protocol.hpp"

namespace alcedo::automation {

/// Blocking JSON-RPC client for one automation session. It is for a process that does not run
/// the session event loop, such as `alcedo-cli`. Do not use it on the thread of the server.
class AutomationClient {
 public:
  /// Connects to @p socket_name. Returns false and sets `error_string()` on failure.
  [[nodiscard]] auto Connect(const QString& socket_name, int timeout_ms) -> bool;

  /// Sends one request and reads lines until the response with the same id arrives.
  /// Notifications that arrive first are ignored. Returns nothing and sets `error_string()`
  /// when the connection closes, a line is invalid, or @p wait_ms passes.
  [[nodiscard]] auto Call(const QString& method, const QJsonObject& params, int wait_ms)
      -> std::optional<AutomationResponse>;

  /// Reads the next notification. A negative @p wait_ms waits without a limit. Returns nothing
  /// and sets `error_string()` when the connection closes or the wait passes.
  [[nodiscard]] auto ReadNotification(int wait_ms) -> std::optional<AutomationNotification>;

  [[nodiscard]] auto error_string() const -> QString { return error_; }
  [[nodiscard]] auto IsConnected() const -> bool;

 private:
  [[nodiscard]] auto ReadMessage(const QDeadlineTimer& deadline)
      -> std::optional<AutomationMessage>;

  QLocalSocket socket_;
  QByteArray   buffer_;
  qint64       next_request_id_ = 1;
  QString      error_;
};

}  // namespace alcedo::automation
