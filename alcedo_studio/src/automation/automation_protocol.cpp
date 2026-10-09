//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_protocol.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>
#include <utility>

namespace alcedo::automation {
namespace {

constexpr auto kJsonRpcVersion = "2.0";

auto IsValidRequestId(const QJsonValue& id) -> bool { return id.isString() || id.isDouble(); }

auto IntegralCode(const QJsonValue& value) -> std::optional<int> {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (std::floor(number) != number || number < -2147483648.0 || number > 2147483647.0) {
    return std::nullopt;
  }
  return static_cast<int>(number);
}

auto MakeParseFailure(AutomationErrorCode code, QString message,
                      QJsonValue id = QJsonValue(QJsonValue::Null)) -> AutomationParseResult {
  AutomationParseResult result;
  result.error = AutomationError{code, std::move(message), QJsonValue(QJsonValue::Undefined)};
  result.id    = std::move(id);
  return result;
}

auto ParseResponseError(const QJsonValue& value) -> std::optional<AutomationError> {
  if (!value.isObject()) {
    return std::nullopt;
  }
  const QJsonObject object = value.toObject();
  const auto        code   = IntegralCode(object.value("code"));
  if (!code.has_value() || !object.value("message").isString()) {
    return std::nullopt;
  }
  AutomationError error;
  // The client keeps codes that this build does not know. The cast is defined because the
  // enumeration has a fixed underlying type.
  error.code    = static_cast<AutomationErrorCode>(*code);
  error.message = object.value("message").toString();
  if (object.contains("data")) {
    error.data = object.value("data");
  }
  return error;
}

auto ErrorToJson(const AutomationError& error) -> QJsonObject {
  QJsonObject object{{"code", static_cast<int>(error.code)}, {"message", error.message}};
  if (!error.data.isUndefined()) {
    object.insert("data", error.data);
  }
  return object;
}

}  // namespace

auto AutomationErrorName(AutomationErrorCode code) -> QString {
  switch (code) {
    case AutomationErrorCode::ParseError:
      return QStringLiteral("parse_error");
    case AutomationErrorCode::InvalidRequest:
      return QStringLiteral("invalid_request");
    case AutomationErrorCode::MethodNotFound:
      return QStringLiteral("method_not_found");
    case AutomationErrorCode::InvalidParams:
      return QStringLiteral("invalid_params");
    case AutomationErrorCode::InternalError:
      return QStringLiteral("internal_error");
    case AutomationErrorCode::NotReady:
      return QStringLiteral("not_ready");
    case AutomationErrorCode::Rejected:
      return QStringLiteral("rejected");
    case AutomationErrorCode::ControlNotHeld:
      return QStringLiteral("control_not_held");
    case AutomationErrorCode::ControlRevokedByUser:
      return QStringLiteral("control_revoked_by_user");
    case AutomationErrorCode::Busy:
      return QStringLiteral("busy");
    case AutomationErrorCode::Timeout:
      return QStringLiteral("timeout");
    case AutomationErrorCode::Failed:
      return QStringLiteral("failed");
  }
  return QStringLiteral("unknown_error");
}

auto AutomationErrorCodeFromInt(int code) -> std::optional<AutomationErrorCode> {
  const auto candidate = static_cast<AutomationErrorCode>(code);
  if (AutomationErrorName(candidate) == QStringLiteral("unknown_error")) {
    return std::nullopt;
  }
  return candidate;
}

auto ParseAutomationLine(QByteArrayView line) -> AutomationParseResult {
  if (line.endsWith('\r')) {
    line.chop(1);
  }

  QJsonParseError     parse_error{};
  const QJsonDocument document = QJsonDocument::fromJson(line.toByteArray(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    return MakeParseFailure(AutomationErrorCode::ParseError,
                            QStringLiteral("invalid JSON at offset %1: %2")
                                .arg(parse_error.offset)
                                .arg(parse_error.errorString()));
  }
  if (!document.isObject()) {
    return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                            QStringLiteral("the message must be a JSON object"));
  }

  const QJsonObject object   = document.object();
  const bool        has_id   = object.contains("id");
  const QJsonValue  id       = object.value("id");
  const QJsonValue  known_id = (has_id && IsValidRequestId(id)) ? id : QJsonValue(QJsonValue::Null);

  if (object.value("jsonrpc") != QJsonValue(kJsonRpcVersion)) {
    return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                            QStringLiteral("the jsonrpc member must be \"2.0\""), known_id);
  }

  if (object.contains("method")) {
    const QJsonValue method = object.value("method");
    if (!method.isString() || method.toString().isEmpty()) {
      return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                              QStringLiteral("the method member must be a non-empty string"),
                              known_id);
    }
    if (has_id && !IsValidRequestId(id)) {
      return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                              QStringLiteral("the id member must be a string or a number"));
    }
    const QJsonValue params = object.value("params");
    if (object.contains("params") && !params.isObject()) {
      return MakeParseFailure(AutomationErrorCode::InvalidParams,
                              QStringLiteral("the params member must be a JSON object"), known_id);
    }

    AutomationParseResult result;
    if (has_id) {
      result.message = AutomationRequest{id, method.toString(), params.toObject()};
      result.id      = id;
    } else {
      result.message = AutomationNotification{method.toString(), params.toObject()};
    }
    return result;
  }

  const bool has_result = object.contains("result");
  const bool has_error  = object.contains("error");
  if (has_result == has_error || !has_id || !(id.isNull() || IsValidRequestId(id))) {
    return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                            QStringLiteral("the message is not a request, a response, or a "
                                           "notification"),
                            known_id);
  }

  AutomationResponse response;
  response.id = id;
  if (has_error) {
    auto error = ParseResponseError(object.value("error"));
    if (!error.has_value()) {
      return MakeParseFailure(AutomationErrorCode::InvalidRequest,
                              QStringLiteral("the error member must have an integer code and a "
                                             "string message"),
                              known_id);
    }
    response.error = std::move(error);
  } else {
    response.result = object.value("result");
  }

  AutomationParseResult result;
  result.message = std::move(response);
  result.id      = id;
  return result;
}

auto SerializeAutomationMessage(const AutomationMessage& message) -> QByteArray {
  QJsonObject object{{"jsonrpc", kJsonRpcVersion}};
  if (const auto* request = std::get_if<AutomationRequest>(&message)) {
    object.insert("id", request->id);
    object.insert("method", request->method);
    object.insert("params", request->params);
  } else if (const auto* response = std::get_if<AutomationResponse>(&message)) {
    object.insert("id", response->id);
    if (response->error.has_value()) {
      object.insert("error", ErrorToJson(*response->error));
    } else {
      object.insert("result", response->result.isUndefined() ? QJsonValue(QJsonValue::Null)
                                                             : response->result);
    }
  } else {
    const auto& notification = std::get<AutomationNotification>(message);
    object.insert("method", notification.method);
    object.insert("params", notification.params);
  }

  QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  bytes.append('\n');
  return bytes;
}

auto MakeAutomationErrorResponse(QJsonValue id, AutomationErrorCode code, QString message,
                                 QJsonValue data) -> AutomationResponse {
  AutomationResponse response;
  response.id    = std::move(id);
  response.error = AutomationError{code, std::move(message), std::move(data)};
  return response;
}

}  // namespace alcedo::automation
