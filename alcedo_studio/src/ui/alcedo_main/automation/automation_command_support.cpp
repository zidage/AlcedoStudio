//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_command_support.hpp"

#include <utility>

namespace alcedo::automation {

auto AutomationObjectSchema(QJsonObject properties, QJsonArray required) -> QJsonObject {
  QJsonObject schema{{"type", "object"}, {"properties", properties}};
  if (!required.isEmpty()) {
    schema.insert("required", required);
  }
  return schema;
}

auto AutomationClosedParamsSchema(QJsonObject properties, QJsonArray required) -> QJsonObject {
  QJsonObject schema = AutomationObjectSchema(std::move(properties), std::move(required));
  schema.insert("additionalProperties", false);
  return schema;
}

void SendAutomationOwnerError(AutomationReply& reply, AutomationErrorCode code,
                              const QString& message) {
  reply.SendError(code, message, QJsonObject{{"reason", message}});
}

void SendAutomationParamError(AutomationReply& reply, const QString& pointer,
                              const QString& message) {
  reply.SendError(AutomationErrorCode::InvalidParams, message,
                  QJsonObject{{"pointer", pointer}, {"reason", message}});
}

AutomationCommandWait::AutomationCommandWait(AutomationReply reply, QObject* parent)
    : QObject(parent), reply_(std::move(reply)) {}

void AutomationCommandWait::SendResult(QJsonValue result) {
  if (finished_) {
    return;
  }
  reply_.SendResult(std::move(result));
  Finish();
}

void AutomationCommandWait::SendError(AutomationErrorCode code, const QString& message,
                                      QJsonValue data) {
  if (finished_) {
    return;
  }
  reply_.SendError(code, message, std::move(data));
  Finish();
}

void AutomationCommandWait::SendOwnerError(AutomationErrorCode code, const QString& message) {
  if (finished_) {
    return;
  }
  SendAutomationOwnerError(reply_, code, message);
  Finish();
}

void AutomationCommandWait::Finish() {
  finished_ = true;
  deleteLater();
}

}  // namespace alcedo::automation
