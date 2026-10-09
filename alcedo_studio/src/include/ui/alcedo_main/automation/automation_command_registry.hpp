//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>
#include <map>
#include <memory>

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
  /// not answered within `timeout_ms`.
  void               Dispatch(const AutomationRequest& request, AutomationReply reply) const;

  /// The command list for `session.describe`, sorted by method name.
  [[nodiscard]] auto Describe() const -> QJsonArray;

  [[nodiscard]] auto Contains(const QString& method) const -> bool;

 private:
  std::map<QString, AutomationCommandSpec> commands_;
};

}  // namespace alcedo::automation
