//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"

namespace alcedo::automation {
namespace {

auto EmptyParamsSchema() -> QJsonObject {
  return QJsonObject{
      {"type", "object"}, {"properties", QJsonObject{}}, {"additionalProperties", false}};
}

}  // namespace

auto RegisterAutomationSessionCommands(AutomationCommandRegistry&   registry,
                                       AutomationSessionDescription description, QString* error)
    -> bool {
  AutomationCommandSpec ping;
  ping.method        = QStringLiteral("session.ping");
  ping.description   = QStringLiteral("Checks that the session answers.");
  ping.params_schema = EmptyParamsSchema();
  ping.result_schema =
      QJsonObject{{"type", "object"},
                  {"properties", QJsonObject{{"pong", QJsonObject{{"type", "boolean"}}}}},
                  {"required", QJsonArray{"pong"}}};
  ping.handler = [](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"pong", true}});
  };
  if (!registry.Register(std::move(ping), error)) {
    return false;
  }

  AutomationCommandSpec describe;
  describe.method      = QStringLiteral("session.describe");
  describe.description = QStringLiteral(
      "Returns the protocol version, the host mode, the application version, and the command "
      "list with the parameter and result schemas.");
  describe.params_schema = EmptyParamsSchema();
  describe.result_schema = QJsonObject{
      {"type", "object"},
      {"properties",
       QJsonObject{
           {"protocol_version", QJsonObject{{"type", "integer"}}},
           {"host_mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"headless", "gui"}}}},
           {"application_version", QJsonObject{{"type", "string"}}},
           {"commands", QJsonObject{{"type", "array"}}}}},
      {"required", QJsonArray{"protocol_version", "host_mode", "application_version", "commands"}}};
  describe.handler = [&registry, description = std::move(description)](const QJsonObject&,
                                                                       AutomationReply reply) {
    reply.SendResult(QJsonObject{{"protocol_version", kAutomationProtocolVersion},
                                 {"host_mode", description.host_mode},
                                 {"application_version", description.application_version},
                                 {"commands", registry.Describe()}});
  };
  return registry.Register(std::move(describe), error);
}

}  // namespace alcedo::automation
