//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <utility>

#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/automation/automation_host_commands.hpp"

namespace alcedo::automation {
namespace {

auto ObjectSchema(QJsonObject properties = {}, QJsonArray required = {}) -> QJsonObject {
  QJsonObject schema{{"type", "object"}, {"properties", properties}};
  if (!required.isEmpty()) {
    schema.insert("required", required);
  }
  return schema;
}

auto ClosedParamsSchema(QJsonObject properties = {}) -> QJsonObject {
  QJsonObject schema = ObjectSchema(std::move(properties));
  schema.insert("additionalProperties", false);
  return schema;
}

auto ReadProjectState(ui::ApplicationModuleHost& host) -> QJsonObject {
  ui::ProjectModule* project = host.project();
  if (project == nullptr) {
    return QJsonObject{{"loaded", false}, {"entered", false}, {"loading", false}, {"path", ""}};
  }
  const auto& handler = project->handler();
  return QJsonObject{{"loaded", project->ServiceReady()},
                     {"entered", project->ProjectEntered()},
                     {"loading", project->ProjectLoading()},
                     {"path", QString::fromStdWString(handler.package_path().generic_wstring())},
                     {"message", project->ServiceMessage()}};
}

auto ReadEditorState(ui::ApplicationModuleHost& host) -> QJsonObject {
  alcedo::EditorSessionService* service = host.editor_session_service();
  if (service == nullptr) {
    return QJsonObject{
        {"state", alcedo::EditorSessionStateName(alcedo::EditorSessionState::NoImage)},
        {"element_id", 0},
        {"image_id", 0}};
  }
  const alcedo::EditorSessionIdentity identity = service->identity();
  return QJsonObject{{"state", alcedo::EditorSessionStateName(service->state())},
                     {"element_id", static_cast<double>(identity.element_id)},
                     {"image_id", static_cast<double>(identity.image_id)}};
}

}  // namespace

auto ReadAutomationSessionState(ui::ApplicationModuleHost& host, const QString& host_mode)
    -> QJsonObject {
  QJsonArray tasks;
  if (ui::BackgroundTaskController* background_tasks = host.background_tasks()) {
    tasks = QJsonArray::fromVariantList(background_tasks->Tasks());
  }
  const QString workspace =
      host.workspace_router() != nullptr ? host.workspace_router()->workspace() : QString();
  // The headless host has no user, so the agent always holds control there.
  const bool headless = host_mode == QStringLiteral("headless");
  return QJsonObject{
      {"host_mode", host_mode},
      {"project", ReadProjectState(host)},
      {"workspace", workspace},
      {"editor", ReadEditorState(host)},
      {"control", QJsonObject{{"held", headless}, {"revoked_by_user", false}, {"holder_name", ""}}},
      {"tasks", tasks},
      {"idle", host.IsIdle()}};
}

auto RegisterAutomationHostCommands(AutomationCommandRegistry&   registry,
                                    AutomationHostCommandContext context, QString* error) -> bool {
  if (context.host == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the host commands need an application module host");
    }
    return false;
  }

  AutomationCommandSpec state;
  state.method      = QStringLiteral("state.get");
  state.description = QStringLiteral(
      "Returns the project, workspace, editor session, control state, running tasks, and "
      "whether all application work is idle.");
  state.params_schema = ClosedParamsSchema();
  state.result_schema = ObjectSchema(
      QJsonObject{{"host_mode", QJsonObject{{"type", "string"}}},
                  {"project", QJsonObject{{"type", "object"}}},
                  {"workspace", QJsonObject{{"type", "string"}}},
                  {"editor", QJsonObject{{"type", "object"}}},
                  {"control", QJsonObject{{"type", "object"}}},
                  {"tasks", QJsonObject{{"type", "array"}}},
                  {"idle", QJsonObject{{"type", "boolean"}}}},
      QJsonArray{"host_mode", "project", "workspace", "editor", "control", "tasks", "idle"});
  state.handler = [host = context.host, host_mode = context.host_mode](const QJsonObject&,
                                                                       AutomationReply reply) {
    reply.SendResult(ReadAutomationSessionState(*host, host_mode));
  };
  if (!registry.Register(std::move(state), error)) {
    return false;
  }

  AutomationCommandSpec shutdown;
  shutdown.method      = QStringLiteral("session.shutdown");
  shutdown.description = QStringLiteral(
      "Shuts the application modules down, which persists the open project, and then ends the "
      "session process. The response arrives after the shutdown.");
  shutdown.changes_state = true;
  // Closing without persistence belongs to project.close. The session shutdown always persists.
  shutdown.params_schema = ClosedParamsSchema(QJsonObject{
      {"persist", QJsonObject{{"type", "boolean"},
                              {"enum", QJsonArray{true}},
                              {"default", true},
                              {"description", "Persist the open project before the exit."}}}});
  shutdown.result_schema = ObjectSchema(
      QJsonObject{{"shut_down", QJsonObject{{"type", "boolean"}}}}, QJsonArray{"shut_down"});
  shutdown.handler = [host = context.host, after_shutdown = std::move(context.after_shutdown)](
                         const QJsonObject&, AutomationReply reply) {
    host->Shutdown();
    reply.SendResult(QJsonObject{{"shut_down", true}});
    if (after_shutdown) {
      after_shutdown();
    }
  };
  return registry.Register(std::move(shutdown), error);
}

}  // namespace alcedo::automation
