//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>
#include <variant>

namespace alcedo::automation {

/// Major version of the automation protocol. A client with a different major value refuses the
/// session.
inline constexpr int       kAutomationProtocolVersion = 1;

/// Longest accepted line, newline excluded. A longer line closes the connection.
inline constexpr qsizetype kMaxAutomationLineBytes    = qsizetype{16} * 1024 * 1024;

/// JSON-RPC 2.0 standard error codes and the Alcedo automation error codes.
enum class AutomationErrorCode : int {
  ParseError           = -32700,
  InvalidRequest       = -32600,
  MethodNotFound       = -32601,
  InvalidParams        = -32602,
  InternalError        = -32603,
  NotReady             = -32001,
  Rejected             = -32002,
  ControlNotHeld       = -32003,
  ControlRevokedByUser = -32004,
  Busy                 = -32005,
  Timeout              = -32006,
  Failed               = -32007,
};

/// Stable snake_case name of an error code, for example `not_ready`.
auto AutomationErrorName(AutomationErrorCode code) -> QString;

/// Maps an integer from the wire to a known code. Returns nothing for an unknown integer.
auto AutomationErrorCodeFromInt(int code) -> std::optional<AutomationErrorCode>;

/// The `error` member of a JSON-RPC response.
struct AutomationError {
  AutomationErrorCode code = AutomationErrorCode::InternalError;
  QString             message;
  /// Optional structured detail. `QJsonValue::Undefined` omits the `data` member.
  QJsonValue          data = QJsonValue(QJsonValue::Undefined);
};

/// A client request. Automation requests always carry an `id` (a string or a number).
struct AutomationRequest {
  QJsonValue  id;
  QString     method;
  QJsonObject params;
};

/// A response to one request. Exactly one of `result` and `error` is present.
struct AutomationResponse {
  /// The request id, or `null` when the request id could not be read.
  QJsonValue                     id     = QJsonValue(QJsonValue::Null);
  QJsonValue                     result = QJsonValue(QJsonValue::Undefined);
  std::optional<AutomationError> error;
};

/// A host-to-client message without an id.
struct AutomationNotification {
  QString     method;
  QJsonObject params;
};

using AutomationMessage =
    std::variant<AutomationRequest, AutomationResponse, AutomationNotification>;

/// Result of parsing one line. On failure `error` is set and `id` holds the request id when the
/// line was a JSON object with a valid id, so that the error response can name the request.
struct AutomationParseResult {
  std::optional<AutomationMessage> message;
  std::optional<AutomationError>   error;
  QJsonValue                       id = QJsonValue(QJsonValue::Null);
};

/// Parses one line without its trailing newline. A trailing carriage return is accepted.
///
/// Errors: invalid JSON gives -32700. A value that is not an object, a missing or wrong
/// `jsonrpc` member, a method that is not a string, an id that is not a string or a number, or an
/// object that is neither a request, a response, nor a notification gives -32600. A `params`
/// member that is not an object gives -32602.
auto ParseAutomationLine(QByteArrayView line) -> AutomationParseResult;

/// Serializes one message as compact JSON followed by `\n`.
auto SerializeAutomationMessage(const AutomationMessage& message) -> QByteArray;

/// Builds an error response for @p id.
auto MakeAutomationErrorResponse(QJsonValue id, AutomationErrorCode code, QString message,
                                 QJsonValue data = QJsonValue(QJsonValue::Undefined))
    -> AutomationResponse;

}  // namespace alcedo::automation
