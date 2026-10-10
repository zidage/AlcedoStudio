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

auto OptionSchema() -> QJsonObject {
  const QJsonObject text{{"type", "string"}};
  return AutomationObjectSchema(QJsonObject{{"value", text}, {"label", text}},
                                QJsonArray{"value", "label"});
}

auto SliderSchema() -> QJsonObject {
  const QJsonObject integer{{"type", "integer"}};
  return AutomationObjectSchema(
      QJsonObject{{"scale", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"kelvin_pivot"}}}},
                  {"position_min", integer},
                  {"position_max", integer},
                  {"pivot_position", integer},
                  {"pivot_kelvin", QJsonObject{{"type", "number"}}}},
      QJsonArray{"scale", "position_min", "position_max", "pivot_position", "pivot_kelvin"});
}

auto PropertySchema() -> QJsonObject {
  const QJsonObject number{{"type", "number"}};
  return AutomationObjectSchema(
      QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                  {"type", QJsonObject{{"type", "string"},
                                       {"enum", QJsonArray{"number", "boolean", "string",
                                                           "option", "number_list"}}}},
                  {"ui_min", number},
                  {"ui_max", number},
                  {"ui_step", number},
                  {"ui_decimals", QJsonObject{{"type", "integer"}}},
                  {"ui_count", QJsonObject{{"type", "integer"}}},
                  {"ui_slider", SliderSchema()},
                  {"options", QJsonObject{{"type", "array"}, {"items", OptionSchema()}}}},
      QJsonArray{"name", "type"});
}

auto EotfBySpaceSchema() -> QJsonObject {
  const QJsonObject values{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}};
  return AutomationObjectSchema(
      QJsonObject{{"rec709", values}, {"p3_d65", values}, {"rec2020", values}},
      QJsonArray{"rec709", "p3_d65", "rec2020"});
}

auto CatalogEntrySchema() -> QJsonObject {
  const QJsonObject number{{"type", "number"}};
  return AutomationObjectSchema(
      QJsonObject{
          {"field", QJsonObject{{"type", "string"}}},
          {"kind",
           QJsonObject{{"type", "string"}, {"enum", QJsonArray{"scalar", "object", "model"}}}},
          {"panel", QJsonObject{{"type", "string"}}},
          {"ui_min", number},
          {"ui_max", number},
          {"ui_default", QJsonObject{{"type", QJsonArray{"number", "object"}}}},
          {"ui_step", number},
          {"ui_decimals", QJsonObject{{"type", "integer"}}},
          {"properties", QJsonObject{{"type", "array"}, {"items", PropertySchema()}}},
          {"model_shape", QJsonObject{{"type", "string"}}},
          {"encoding_eotf_by_space", EotfBySpaceSchema()},
          {"aliases",
           QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}},
      QJsonArray{"field", "kind", "panel", "ui_default"});
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
      "shown decimals. An object field lists its properties (number range, boolean, string, or "
      "option list, or a list of numbers) and its default object; a write may give some of the "
      "properties. A model field (curve, lut) takes its Model JSON, described in model_shape; a "
      "write gives the complete value. A Kelvin "
      "property also has its slider position scale (ui_slider), and odt lists the valid "
      "encoding_eotf values of each encoding_space. A value outside the range or the option "
      "list is rejected; it is not clamped.");
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
