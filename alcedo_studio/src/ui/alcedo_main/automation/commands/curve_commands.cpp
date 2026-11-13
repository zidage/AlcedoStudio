//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <string_view>
#include <utility>

#include "edit/operators/models/tone_curve_presets.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_curve_commands.hpp"

namespace alcedo::automation {
namespace {

auto Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto PresetObject(const ToneCurvePreset& preset) -> QJsonObject {
  QJsonArray points;
  for (const auto& point : preset.points) {
    points.push_back(
        QJsonObject{{"x", static_cast<double>(point.x)}, {"y", static_cast<double>(point.y)}});
  }
  return QJsonObject{{"id", Text(preset.id)},
                     {"name", Text(preset.display_name)},
                     {"group", Text(preset.group)},
                     {"points", points},
                     {"editor_value", QJsonObject{{"points", Text(preset.id)}}}};
}

auto ResultSchema() -> QJsonObject {
  const QJsonObject text{{"type", "string"}};
  const QJsonObject number{{"type", "number"}};
  const QJsonObject point =
      AutomationObjectSchema(QJsonObject{{"x", number}, {"y", number}}, QJsonArray{"x", "y"});
  const QJsonObject preset = AutomationObjectSchema(
      QJsonObject{{"id", text},
                  {"name", text},
                  {"group", QJsonObject{{"type", "string"},
                                        {"enum", QJsonArray{"", "Contrast", "Rendering", "Film"}}}},
                  {"points", QJsonObject{{"type", "array"}, {"items", point}}},
                  {"editor_value", QJsonObject{{"type", "object"}}}},
      QJsonArray{"id", "name", "group", "points", "editor_value"});
  return AutomationObjectSchema(
      QJsonObject{{"total", QJsonObject{{"type", "integer"}}},
                  {"presets", QJsonObject{{"type", "array"}, {"items", preset}}}},
      QJsonArray{"total", "presets"});
}

}  // namespace

auto RegisterAutomationCurveCommands(AutomationCommandRegistry& registry, QString* error) -> bool {
  AutomationCommandSpec list;
  list.method      = QStringLiteral("curve.presets");
  list.description = QStringLiteral(
      "Lists the Tone Curve presets in the order of the preset menu: Linear, the contrast "
      "shapes, the rendering looks (Film-like is the curve of the RAW import look), and the "
      "films. 'points' are the control points that a preset writes, on the curve axis (ACEScc, "
      "18% grey at 0.41359). 'editor_value' applies the preset to the RGB curve: give it to "
      "editor.set on 'curve'. The id also applies to a channel curve, for example "
      "{\"red\": \"kodak_2383\"}. The document stores the points, not the id. Needs no open "
      "project.");
  list.params_schema = AutomationClosedParamsSchema(QJsonObject{});
  list.result_schema = ResultSchema();
  list.handler       = [](const QJsonObject&, AutomationReply reply) {
    QJsonArray presets;
    for (const auto& preset : ToneCurvePresets()) {
      presets.push_back(PresetObject(preset));
    }
    reply.SendResult(QJsonObject{{"total", presets.size()}, {"presets", presets}});
  };
  return registry.Register(std::move(list), error);
}

}  // namespace alcedo::automation
