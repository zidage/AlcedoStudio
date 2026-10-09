//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <utility>

#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_task_commands.hpp"

namespace alcedo::automation {
namespace {

auto TaskSchema() -> QJsonObject {
  return AutomationObjectSchema(QJsonObject{{"task_id", QJsonObject{{"type", "string"}}},
                                            {"kind", QJsonObject{{"type", "string"}}},
                                            {"state", QJsonObject{{"type", "string"}}},
                                            {"progress", QJsonObject{{"type", "integer"}}},
                                            {"message", QJsonObject{{"type", "string"}}},
                                            {"counts", QJsonObject{{"type", "object"}}}},
                                QJsonArray{"task_id", "kind", "state"});
}

/// Answers @p wait when the application is idle. The check runs queued, so work that a signal
/// handler queues in the same event loop turn (for example the post-import processing) counts.
void AnswerWhenIdle(ui::ApplicationModuleHost* host, AutomationCommandWait* wait) {
  const auto check = [host, wait]() {
    QMetaObject::invokeMethod(
        wait,
        [host, wait]() {
          if (host->IsIdle()) {
            wait->SendResult(QJsonObject{{"idle", true}});
          }
        },
        Qt::QueuedConnection);
  };
  QObject::connect(host->import_export(), &ui::ImportExportHandler::ImportStateChanged, wait,
                   check);
  QObject::connect(host->import_export(), &ui::ImportExportHandler::ExportStateChanged, wait,
                   check);
  QObject::connect(host->background_tasks(), &ui::BackgroundTaskController::TasksChanged, wait,
                   check);
  check();
}

}  // namespace

auto RegisterAutomationTaskCommands(AutomationCommandRegistry& registry,
                                    ui::ApplicationModuleHost* host, AutomationTaskTracker* tracker,
                                    QString* error) -> bool {
  if (host == nullptr || tracker == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the task commands need a host and a task tracker");
    }
    return false;
  }

  AutomationCommandSpec list;
  list.method      = QStringLiteral("tasks.list");
  list.description = QStringLiteral(
      "Returns the tasks that have not finished (imports, an export, and background tasks) and "
      "whether all application work is idle.");
  list.params_schema = AutomationClosedParamsSchema();
  list.result_schema = AutomationObjectSchema(
      QJsonObject{{"tasks", QJsonObject{{"type", "array"}, {"items", TaskSchema()}}},
                  {"idle", QJsonObject{{"type", "boolean"}}}},
      QJsonArray{"tasks", "idle"});
  list.handler = [host, tracker](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"tasks", tracker->RunningTasks()}, {"idle", host->IsIdle()}});
  };
  if (!registry.Register(std::move(list), error)) {
    return false;
  }

  AutomationCommandSpec wait;
  wait.method      = QStringLiteral("tasks.wait");
  wait.description = QStringLiteral(
      "Waits until the task 'task_id' finishes and returns its final state, or with 'idle': "
      "true until no import, export, or background task runs.");
  wait.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"task_id", QJsonObject{{"type", "string"}, {"description", "A task id from a command."}}},
      {"idle", QJsonObject{{"type", "boolean"},
                           {"description", "Wait for all application work instead."}}}});
  wait.result_schema = AutomationObjectSchema();
  wait.handler       = [host, tracker](const QJsonObject& params, AutomationReply reply) {
    const bool    idle    = params.value("idle").toBool(false);
    const QString task_id = params.value("task_id").toString();
    if (idle == !task_id.isEmpty()) {
      SendAutomationParamError(reply, QString(),
                                     QStringLiteral("give exactly one of task_id and idle: true"));
      return;
    }
    if (idle) {
      AnswerWhenIdle(host, new AutomationCommandWait(std::move(reply), host));
      return;
    }
    const auto task = tracker->Task(task_id);
    if (!task.has_value()) {
      SendAutomationParamError(reply, QStringLiteral("/task_id"),
                                     QStringLiteral("unknown task id: %1").arg(task_id));
      return;
    }
    if (AutomationTaskTracker::IsTerminalState(task->value("state").toString())) {
      reply.SendResult(*task);
      return;
    }
    auto* pending = new AutomationCommandWait(std::move(reply), host);
    QObject::connect(tracker, &AutomationTaskTracker::TaskFinished, pending,
                           [pending, task_id](const QJsonObject& finished) {
                       if (finished.value("task_id").toString() == task_id) {
                         pending->SendResult(finished);
                       }
                     });
  };
  return registry.Register(std::move(wait), error);
}

}  // namespace alcedo::automation
