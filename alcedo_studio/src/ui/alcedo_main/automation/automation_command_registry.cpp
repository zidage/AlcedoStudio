//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_command_registry.hpp"

#include <QDebug>
#include <QTimer>
#include <chrono>
#include <exception>
#include <utility>

#include "ui/alcedo_main/automation/automation_json_schema.hpp"

namespace alcedo::automation {
namespace {

constexpr auto kTimeoutParameter = "timeout_ms";

auto           TimeoutParameterSchema() -> QJsonObject {
  return QJsonObject{
                {"type", "integer"},
                {"minimum", 1},
                {"description", "Time limit in milliseconds for the command to reach its terminal state."},
                {"default", kDefaultAutomationTimeoutMs}};
}

void SetError(QString* error, QString message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

AutomationReply::AutomationReply(QJsonValue id, ResponseSink sink)
    : state_(std::make_shared<State>(State{std::move(id), std::move(sink), false})) {}

AutomationReply::~AutomationReply() { SendDroppedErrorIfPending(); }

AutomationReply::AutomationReply(AutomationReply&& other) noexcept
    : state_(std::move(other.state_)) {}

auto AutomationReply::operator=(AutomationReply&& other) noexcept -> AutomationReply& {
  if (this != &other) {
    SendDroppedErrorIfPending();
    state_ = std::move(other.state_);
  }
  return *this;
}

void AutomationReply::SendResult(QJsonValue result) {
  if (!state_) {
    return;
  }
  AutomationResponse response;
  response.id     = state_->id;
  response.result = std::move(result);
  Send(*state_, std::move(response));
}

void AutomationReply::SendError(AutomationErrorCode code, QString message, QJsonValue data) {
  if (!state_) {
    return;
  }
  Send(*state_, MakeAutomationErrorResponse(state_->id, code, std::move(message), std::move(data)));
}

auto AutomationReply::IsSent() const -> bool { return state_ && state_->sent; }

auto AutomationReply::request_id() const -> QJsonValue {
  return state_ ? state_->id : QJsonValue(QJsonValue::Null);
}

void AutomationReply::Send(State& state, AutomationResponse response) {
  if (state.sent) {
    return;
  }
  state.sent = true;
  if (state.sink) {
    state.sink(std::move(response));
  }
}

void AutomationReply::SendDroppedErrorIfPending() {
  if (state_ && !state_->sent) {
    Send(*state_, MakeAutomationErrorResponse(state_->id, AutomationErrorCode::InternalError,
                                              QStringLiteral("handler dropped the request")));
  }
  state_.reset();
}

auto AutomationCommandRegistry::Register(AutomationCommandSpec spec, QString* error) -> bool {
  if (spec.method.isEmpty()) {
    SetError(error, QStringLiteral("a command needs a method name"));
    return false;
  }
  if (!spec.handler) {
    SetError(error, QStringLiteral("command '%1' has no handler").arg(spec.method));
    return false;
  }
  if (commands_.contains(spec.method)) {
    SetError(error, QStringLiteral("command '%1' is already registered").arg(spec.method));
    return false;
  }
  if (spec.params_schema.value("type") != QJsonValue("object") ||
      spec.params_schema.value("additionalProperties") != QJsonValue(false)) {
    SetError(error, QStringLiteral("the parameter schema of '%1' must be an object schema with "
                                   "\"additionalProperties\": false")
                        .arg(spec.method));
    return false;
  }
  if (spec.result_schema.isEmpty()) {
    SetError(error, QStringLiteral("command '%1' has no result schema").arg(spec.method));
    return false;
  }
  for (const auto& [schema, name] : {std::pair{&spec.params_schema, QStringLiteral("parameter")},
                                     std::pair{&spec.result_schema, QStringLiteral("result")}}) {
    if (const auto violation = CheckAutomationSchemaKeywords(*schema)) {
      SetError(error, QStringLiteral("the %1 schema of '%2' is invalid at '%3': %4")
                          .arg(name, spec.method, violation->pointer, violation->reason));
      return false;
    }
  }

  QJsonObject properties = spec.params_schema.value("properties").toObject();
  if (!properties.contains(kTimeoutParameter)) {
    properties.insert(kTimeoutParameter, TimeoutParameterSchema());
    spec.params_schema.insert("properties", properties);
  }

  const QString method = spec.method;
  commands_.emplace(method, std::move(spec));
  return true;
}

void AutomationCommandRegistry::Dispatch(const AutomationRequest& request,
                                         AutomationReply          reply) const {
  const auto command = commands_.find(request.method);
  if (command == commands_.end()) {
    reply.SendError(AutomationErrorCode::MethodNotFound,
                    QStringLiteral("unknown method '%1'").arg(request.method),
                    QJsonObject{{"method", request.method}});
    return;
  }

  const AutomationCommandSpec& spec = command->second;
  if (const auto violation = ValidateAutomationJson(request.params, spec.params_schema)) {
    reply.SendError(
        AutomationErrorCode::InvalidParams,
        QStringLiteral("invalid parameter at '%1': %2").arg(violation->pointer, violation->reason),
        QJsonObject{{"pointer", violation->pointer}, {"reason", violation->reason}});
    return;
  }

  const int timeout_ms = request.params.value(kTimeoutParameter).toInt(kDefaultAutomationTimeoutMs);
  std::weak_ptr<AutomationReply::State> pending = reply.state_;
  QTimer::singleShot(std::chrono::milliseconds(timeout_ms), [pending, timeout_ms]() {
    if (const auto state = pending.lock()) {
      AutomationReply::Send(
          *state,
          MakeAutomationErrorResponse(
              state->id, AutomationErrorCode::Timeout,
              QStringLiteral("the command did not finish within %1 ms; the operation can still "
                             "finish later")
                  .arg(timeout_ms),
              QJsonObject{{"timeout_ms", timeout_ms}}));
    }
  });

  try {
    spec.handler(request.params, std::move(reply));
  } catch (const std::exception& exception) {
    // The reply was destroyed during stack unwinding and sent the dropped-request error.
    qWarning().noquote() << "automation command" << request.method
                         << "threw an exception:" << exception.what();
  }
}

auto AutomationCommandRegistry::Describe() const -> QJsonArray {
  QJsonArray commands;
  for (const auto& [method, spec] : commands_) {
    commands.push_back(QJsonObject{{"method", method},
                                   {"description", spec.description},
                                   {"changes_state", spec.changes_state},
                                   {"params_schema", spec.params_schema},
                                   {"result_schema", spec.result_schema}});
  }
  return commands;
}

auto AutomationCommandRegistry::Contains(const QString& method) const -> bool {
  return commands_.contains(method);
}

}  // namespace alcedo::automation
