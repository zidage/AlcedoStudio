// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include <stdexcept>

#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_project_commands.hpp"

namespace alcedo::automation {
namespace {
auto Summary(ui::ProjectMaintenanceController& controller, int page = 0, int filter = -1,
             const QString& search = {}) -> QJsonObject {
  int        count = 0;
  const auto rows  = controller.Model()->ReadFindingsPage(page, filter, search, count);
  return {{"state", controller.State()},
          {"message", controller.Message()},
          {"removed_count", controller.RemovedCount()},
          {"retained_count", controller.RetainedCount()},
          {"backup_path", controller.BackupPath()},
          {"matching_count", count},
          {"page", page},
          {"findings", QJsonArray::fromVariantList(rows)}};
}
auto SummarySchema() -> QJsonObject {
  const auto integer = QJsonObject{{"type", "integer"}};
  const auto text    = QJsonObject{{"type", "string"}};
  const auto boolean = QJsonObject{{"type", "boolean"}};
  const auto row =
      AutomationObjectSchema({{"entryId", integer},
                              {"sourcePath", text},
                              {"rating", integer},
                              {"editingWork", text},
                              {"findingKind", integer},
                              {"reason", text},
                              {"retainedId", QJsonObject{{"type", QJsonArray{"integer", "null"}}}},
                              {"removable", boolean},
                              {"selected", boolean}},
                             {"entryId", "sourcePath", "rating", "editingWork", "findingKind",
                              "reason", "retainedId", "removable", "selected"});
  return AutomationObjectSchema(
      {{"state", text},
       {"message", text},
       {"removed_count", integer},
       {"retained_count", integer},
       {"backup_path", text},
       {"matching_count", integer},
       {"page", integer},
       {"findings", QJsonObject{{"type", "array"}, {"maxItems", 100}, {"items", row}}}},
      {"state", "message", "removed_count", "retained_count", "backup_path", "matching_count",
       "page", "findings"});
}
}  // namespace
auto RegisterAutomationMaintenanceCommands(AutomationCommandRegistry& registry,
                                           ui::ApplicationModuleHost* host, QString* error)
    -> bool {
  auto* controller = host->maintenance();
  registry.SetProjectWriteRefusal([controller] {
    return controller->Busy() && !controller->Scanning()
               ? QStringLiteral(
                     "Project Maintenance reserves project writes until activation or recovery "
                     "completes.")
               : QString{};
  });
  AutomationCommandSpec scan;
  scan.method      = QStringLiteral("project.maintenance.scan");
  scan.description = QStringLiteral(
      "Scans ordinary photos in the loaded project. Returns the first bounded "
      "findings page after completion.");
  scan.params_schema = AutomationClosedParamsSchema();
  scan.result_schema = SummarySchema();
  scan.handler       = [host, controller](const QJsonObject&, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) return;
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    QObject::connect(controller, &ui::ProjectMaintenanceController::ScanFinished, wait,
                     [controller, wait](bool complete) {
                       if (complete)
                         wait->SendResult(Summary(*controller));
                       else
                         wait->SendOwnerError(AutomationErrorCode::Failed,
                                              controller->Error().isEmpty() ? controller->Message()
                                                                            : controller->Error());
                     });
    if (!controller->Scan())
      wait->SendOwnerError(AutomationErrorCode::Busy,
                           QStringLiteral("Maintenance scan cannot start now."));
  };
  if (!registry.Register(std::move(scan), error)) return false;
  AutomationCommandSpec findings;
  findings.method      = QStringLiteral("project.maintenance.findings");
  findings.description = QStringLiteral(
      "Reads a bounded review page. Filters never change the explicit selected IDs.");
  findings.params_schema = AutomationClosedParamsSchema(
      {{"page", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
       {"filter", QJsonObject{{"type", "integer"}, {"minimum", -1}, {"maximum", 5}}},
       {"search", QJsonObject{{"type", "string"}}}});
  findings.result_schema = SummarySchema();
  findings.handler       = [controller](const QJsonObject& params, AutomationReply reply) {
    if (controller->Busy()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Busy,
                               QStringLiteral("Maintenance is running."));
      return;
    }
    try {
      reply.SendResult(Summary(*controller, params.value("page").toInt(),
                               params.value("filter").toInt(-1),
                               params.value("search").toString()));
    } catch (const std::out_of_range& error) {
      SendAutomationParamError(reply, QStringLiteral("/page"), QString::fromUtf8(error.what()));
    }
  };
  if (!registry.Register(std::move(findings), error)) return false;
  AutomationCommandSpec remove;
  remove.method      = QStringLiteral("project.maintenance.remove_selected");
  remove.description = QStringLiteral(
      "Removes only explicit reviewed pristine entry IDs after verified backup. Preserves sources "
      "and protected work. Completes after package reopening or reports the recovery error.");
  remove.changes_state = true;
  remove.params_schema = AutomationClosedParamsSchema(
      {{"element_ids", QJsonObject{{"type", "array"},
                                   {"minItems", 1},
                                   {"items", QJsonObject{{"type", "integer"}, {"minimum", 1}}}}},
       {"backup_directory", QJsonObject{{"type", "string"}}}},
      {"element_ids"});
  remove.result_schema = SummarySchema();
  remove.handler       = [host, controller](const QJsonObject& params, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) return;
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    QObject::connect(controller, &ui::ProjectMaintenanceController::Finished, wait,
                     [controller, wait](bool success) {
                       if (success)
                         wait->SendResult(Summary(*controller));
                       else
                         wait->SendOwnerError(AutomationErrorCode::Failed, controller->Error());
                     });
    if (!controller->ApplyIds(params.value("element_ids").toArray().toVariantList(),
                              params.value("backup_directory").toString())) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, controller->Error());
    }
  };
  return registry.Register(std::move(remove), error);
}
}  // namespace alcedo::automation
