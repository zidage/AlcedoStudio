//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

#include "app/editor_parameter_catalog.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"

namespace alcedo::automation {
namespace {

auto CatalogEntrySchema() -> QJsonObject {
  const QJsonObject number{{"type", "number"}};
  return AutomationObjectSchema(
      QJsonObject{{"field", QJsonObject{{"type", "string"}}},
                  {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"scalar"}}}},
                  {"panel", QJsonObject{{"type", "string"}}},
                  {"ui_min", number},
                  {"ui_max", number},
                  {"ui_default", number},
                  {"ui_step", number},
                  {"ui_decimals", QJsonObject{{"type", "integer"}}},
                  {"aliases", QJsonObject{{"type", "array"},
                                          {"items", QJsonObject{{"type", "string"}}}}}},
      QJsonArray{"field", "kind", "panel"});
}

}  // namespace

auto AutomationEditorCatalogFields() -> QJsonArray {
  const std::string text = EditorParameterCatalog::CatalogJson().dump();
  return QJsonDocument::fromJson(QByteArray::fromStdString(text)).array();
}

auto RegisterAutomationEditorCommands(AutomationCommandRegistry& registry, QString* error)
    -> bool {
  AutomationCommandSpec catalog;
  catalog.method      = QStringLiteral("editor.catalog");
  catalog.description = QStringLiteral(
      "Returns every editor field that editor.set accepts, in UI units: the values that the "
      "editor panels show. A scalar field has its range (ui_min, ui_max), default, step, and "
      "shown decimals. A value outside the range is rejected; it is not clamped.");
  catalog.params_schema = AutomationClosedParamsSchema();
  catalog.result_schema = AutomationObjectSchema(
      QJsonObject{{"fields", QJsonObject{{"type", "array"}, {"items", CatalogEntrySchema()}}}},
      QJsonArray{"fields"});
  catalog.handler = [](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"fields", AutomationEditorCatalogFields()}});
  };
  return registry.Register(std::move(catalog), error);
}

}  // namespace alcedo::automation
