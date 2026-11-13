//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "automation/automation_protocol.hpp"

namespace alcedo::automation {

/// Default value of the `timeout_ms` parameter that every command accepts.
inline constexpr int kDefaultAutomationTimeoutMs = 120000;

/// Sends exactly one response for one request. Move-only.
///
/// The first `SendResult` or `SendError` call sends the response; later calls do nothing. When
/// the last owner destroys a reply that has not sent a response, the destructor sends -32603
/// with the reason "handler dropped the request". GUI thread only.
class AutomationReply {
 public:
  using ResponseSink = std::function<void(AutomationResponse)>;

  AutomationReply()  = default;
  AutomationReply(QJsonValue id, ResponseSink sink);
  ~AutomationReply();

  AutomationReply(AutomationReply&& other) noexcept;
  auto operator=(AutomationReply&& other) noexcept -> AutomationReply&;
  AutomationReply(const AutomationReply&)                                  = delete;
  auto               operator=(const AutomationReply&) -> AutomationReply& = delete;

  void               SendResult(QJsonValue result);
  void               SendError(AutomationErrorCode code, QString message,
                               QJsonValue data = QJsonValue(QJsonValue::Undefined));

  /// True after a response was sent, including a timeout response from the registry.
  [[nodiscard]] auto IsSent() const -> bool;
  [[nodiscard]] auto request_id() const -> QJsonValue;

 private:
  friend class AutomationCommandRegistry;

  struct State {
    QJsonValue   id;
    ResponseSink sink;
    bool         sent = false;
  };

  static void            Send(State& state, AutomationResponse response);
  void                   SendDroppedErrorIfPending();

  std::shared_ptr<State> state_;
};

/// One named command. `params_schema` and `result_schema` use the JSON Schema subset of
/// `CheckAutomationSchemaKeywords`.
struct AutomationCommandSpec {
  QString                                                               method;
  QString                                                               description;
  QJsonObject                                                           params_schema;
  QJsonObject                                                           result_schema;
  /// True when the command changes application state. In a GUI session such a command needs
  /// agent control.
  bool                                                                  changes_state = false;
  std::function<void(const QJsonObject& params, AutomationReply reply)> handler;
};

/// The error that refuses a state-changing command before its handler runs.
struct AutomationCommandRefusal {
  AutomationErrorCode code = AutomationErrorCode::ControlNotHeld;
  QString             message;
  QJsonObject         data;
};

/// Agent control rules for the state-changing commands of a GUI session. A headless session has
/// no user and sets none, so every command is admitted.
struct AutomationStateChangeControl {
  /// Returns a refusal when a state-changing command cannot start now.
  std::function<std::optional<AutomationCommandRefusal>()> admit;
  /// Called with the command summary and the `note` parameter of the request (empty without
  /// one) before the handler of an admitted command. Returns a token for `end_command`.
  std::function<quint64(const QString& summary, const QString& note)> begin_command;
  /// Called after the response of the command of @p token was sent. @p succeeded is false when
  /// the response is an error, also a timeout error.
  std::function<void(quint64 token, bool succeeded)>                  end_command;
};

/// A short text that names @p request for a person: the method and the scalar parameter values
/// in key order, without `timeout_ms` and `note`. An array shows its item count. For example
/// `editor.set saturation 20`.
auto AutomationCommandSummary(const AutomationRequest& request) -> QString;

/// Session-lifetime table of named commands. GUI thread only.
class AutomationCommandRegistry {
 public:
  /// Adds @p spec. Fails for an empty method name, a missing handler, a duplicate method, a
  /// parameter schema that is not a closed object (`"type": "object"` and
  /// `"additionalProperties": false`), or an unsupported schema keyword. Adds the optional
  /// `timeout_ms` parameter to the parameter schema.
  [[nodiscard]] auto Register(AutomationCommandSpec spec, QString* error = nullptr) -> bool;

  /// Validates the request parameters and calls the handler. Sends -32601 for an unknown method
  /// and -32602 with `data.pointer` for invalid parameters. Sends -32006 when the handler has
  /// not answered within `timeout_ms`. A state-changing command first goes through the state
  /// change control: a refusal is sent instead of a handler call.
  void               Dispatch(const AutomationRequest& request, AutomationReply reply) const;

  /// The command list for `session.describe`, sorted by method name.
  [[nodiscard]] auto Describe() const -> QJsonArray;

  [[nodiscard]] auto Contains(const QString& method) const -> bool;

  /// Sets the agent control rules of the state-changing commands. Call before the first
  /// dispatch.
  void               SetStateChangeControl(AutomationStateChangeControl control);
  /// The refusal of a state-changing command now, or nothing. Batch handlers call it again
  /// between their steps.
  [[nodiscard]] auto AdmitStateChange() const -> std::optional<AutomationCommandRefusal>;

 private:
  std::map<QString, AutomationCommandSpec> commands_;
  AutomationStateChangeControl             state_change_control_;
};

}  // namespace alcedo::automation
