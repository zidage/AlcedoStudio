//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_client.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace alcedo::automation {
namespace {

auto MakeDeadline(int wait_ms) -> QDeadlineTimer {
  return wait_ms < 0 ? QDeadlineTimer(QDeadlineTimer::Forever) : QDeadlineTimer(wait_ms);
}

}  // namespace

auto AutomationClient::Connect(const QString& socket_name, int timeout_ms) -> bool {
  socket_.connectToServer(socket_name);
  if (socket_.waitForConnected(timeout_ms)) {
    return true;
  }
  error_ =
      QStringLiteral("cannot connect to socket '%1': %2").arg(socket_name, socket_.errorString());
  return false;
}

auto AutomationClient::IsConnected() const -> bool {
  return socket_.state() == QLocalSocket::ConnectedState;
}

auto AutomationClient::Call(const QString& method, const QJsonObject& params, int wait_ms)
    -> std::optional<AutomationResponse> {
  const QJsonValue id(static_cast<double>(next_request_id_++));
  socket_.write(SerializeAutomationMessage(AutomationRequest{id, method, params}));

  const QDeadlineTimer deadline = MakeDeadline(wait_ms);
  while (true) {
    std::optional<AutomationMessage> message = ReadMessage(deadline);
    if (!message.has_value()) {
      return std::nullopt;
    }
    if (auto* response = std::get_if<AutomationResponse>(&*message)) {
      if (response->id == id || response->id.isNull()) {
        return std::move(*response);
      }
    }
  }
}

auto AutomationClient::ReadNotification(int wait_ms) -> std::optional<AutomationNotification> {
  const QDeadlineTimer deadline = MakeDeadline(wait_ms);
  while (true) {
    std::optional<AutomationMessage> message = ReadMessage(deadline);
    if (!message.has_value()) {
      return std::nullopt;
    }
    if (auto* notification = std::get_if<AutomationNotification>(&*message)) {
      return std::move(*notification);
    }
  }
}

auto AutomationClient::ReadMessage(const QDeadlineTimer& deadline)
    -> std::optional<AutomationMessage> {
  while (true) {
    const qsizetype newline = buffer_.indexOf('\n');
    if (newline >= 0) {
      const QByteArray line = buffer_.left(newline);
      buffer_.remove(0, newline + 1);
      if (line.trimmed().isEmpty()) {
        continue;
      }
      AutomationParseResult parsed = ParseAutomationLine(line);
      if (parsed.error.has_value()) {
        error_ = QStringLiteral("the session sent an invalid line: %1").arg(parsed.error->message);
        return std::nullopt;
      }
      return std::move(parsed.message);
    }

    if (socket_.bytesAvailable() > 0) {
      buffer_.append(socket_.readAll());
      continue;
    }
    if (socket_.state() != QLocalSocket::ConnectedState) {
      error_ = QStringLiteral("the session closed the connection");
      return std::nullopt;
    }
    if (deadline.hasExpired()) {
      error_ = QStringLiteral("no answer from the session within the wait time");
      return std::nullopt;
    }
    const qint64 remaining = deadline.remainingTime();
    // Wait in steps so that a closed connection is seen without a long block.
    const int step_ms = remaining < 0 ? 1000 : static_cast<int>(std::min<qint64>(remaining, 1000));
    if (socket_.waitForReadyRead(step_ms)) {
      buffer_.append(socket_.readAll());
    }
  }
}

}  // namespace alcedo::automation
