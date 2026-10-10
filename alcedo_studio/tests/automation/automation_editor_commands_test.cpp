//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// editor.catalog through AutomationCommandRegistry::Dispatch, the path of every socket request.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <utility>
#include <vector>

#include "app/editor_parameter_catalog.hpp"
#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"

namespace alcedo::automation {
namespace {

/// Dispatches one request and returns the responses that it sent.
auto Dispatch(const AutomationCommandRegistry& registry, const QString& method,
              QJsonObject params = {}) -> std::vector<AutomationResponse> {
  std::vector<AutomationResponse> responses;
  registry.Dispatch(AutomationRequest{QJsonValue(1), method, std::move(params)},
                    AutomationReply(QJsonValue(1), [&responses](AutomationResponse response) {
                      responses.push_back(std::move(response));
                    }));
  return responses;
}

auto FindField(const QJsonArray& fields, const QString& name) -> QJsonObject {
  for (const auto& field : fields) {
    if (field.toObject().value("field").toString() == name) {
      return field.toObject();
    }
  }
  return {};
}

TEST(AutomationEditorCommandsTest, EditorCatalogListsEveryFieldInUiUnits) {
  AutomationCommandRegistry registry;
  QString                   error;
  ASSERT_TRUE(RegisterAutomationEditorCatalogCommand(registry, &error)) << error.toStdString();

  const auto responses = Dispatch(registry, QStringLiteral("editor.catalog"));
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_FALSE(responses[0].error.has_value()) << responses[0].error->message.toStdString();
  const QJsonArray fields = responses[0].result.toObject().value("fields").toArray();
  ASSERT_EQ(static_cast<std::size_t>(fields.size()), EditorParameterCatalog::Entries().size());
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    const QJsonObject field = FindField(fields, QString::fromUtf8(entry.field.data(),
                                                                  static_cast<int>(entry.field.size())));
    ASSERT_FALSE(field.isEmpty()) << entry.field;
    if (entry.kind == EditorParameterValueKind::Model) {
      EXPECT_EQ(field.value("kind").toString(), QStringLiteral("model"));
      EXPECT_FALSE(field.value("model_shape").toString().isEmpty()) << entry.field;
      EXPECT_TRUE(field.value("ui_default").isObject()) << entry.field;
      continue;
    }
    if (entry.kind == EditorParameterValueKind::Object) {
      EXPECT_EQ(field.value("kind").toString(), QStringLiteral("object"));
      EXPECT_EQ(static_cast<std::size_t>(field.value("properties").toArray().size()),
                entry.properties.size())
          << entry.field;
      EXPECT_TRUE(field.value("ui_default").isObject()) << entry.field;
      continue;
    }
    EXPECT_EQ(field.value("kind").toString(), QStringLiteral("scalar"));
    EXPECT_DOUBLE_EQ(field.value("ui_min").toDouble(), entry.range.minimum) << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_max").toDouble(), entry.range.maximum) << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_default").toDouble(), entry.range.default_value)
        << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_step").toDouble(), entry.range.step) << entry.field;
  }
  const QJsonObject saturation = FindField(fields, QStringLiteral("saturation"));
  EXPECT_EQ(saturation.value("panel").toString(), QStringLiteral("look"));
  EXPECT_DOUBLE_EQ(saturation.value("ui_min").toDouble(), -100.0);
  EXPECT_DOUBLE_EQ(saturation.value("ui_max").toDouble(), 100.0);
  const QJsonObject raw = FindField(fields, QStringLiteral("raw_decode"));
  EXPECT_EQ(raw.value("panel").toString(), QStringLiteral("raw"));
  const QJsonObject method = raw.value("properties").toArray().first().toObject();
  EXPECT_EQ(method.value("name").toString(), QStringLiteral("method"));
  EXPECT_EQ(method.value("type").toString(), QStringLiteral("option"));
  EXPECT_EQ(method.value("options").toArray().size(), 3);
}

TEST(AutomationEditorCommandsTest, EditorCatalogRejectsUnknownParameter) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(RegisterAutomationEditorCatalogCommand(registry));

  const auto responses =
      Dispatch(registry, QStringLiteral("editor.catalog"), QJsonObject{{"field", "saturation"}});
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_TRUE(responses[0].error.has_value());
  EXPECT_EQ(responses[0].error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(responses[0].error->data.toObject().value("pointer").toString(),
            QStringLiteral("/field"));
}

}  // namespace
}  // namespace alcedo::automation
