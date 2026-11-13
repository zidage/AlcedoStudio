//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <filesystem>

#include "ui/alcedo_main/album_backend/export_recipe_builder.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto SourceQmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: harnessRoot
    width: 360
    height: 700
    visible: true
    color: appTheme.bgCanvasColor

    Loader {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        source: editorSourceUrl
        onLoaded: {
            item.objectName = "exportNamingEditor"
            item.sampleSourceName = "DSCF2074.RAF"
            item.outputExtension = ".jpg"
            item.patternRules = exportRecipe
        }
    }
}
)";

struct NamingEditorHarness {
  // The pattern rules of the export panel; no export starts in these tests.
  ExportRecipeBuilder   export_recipe{nullptr};
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           editor = nullptr;
  QStringList           warnings;

  NamingEditorHarness() {
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& messages) {
      for (const auto& message : messages) warnings.push_back(message.toString());
    });
    engine.addImportPath(SourceQmlDirectory());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("exportRecipe"), &export_recipe);
    engine.rootContext()->setContextProperty(
        QStringLiteral("editorSourceUrl"),
        QUrl::fromLocalFile(SourceQmlDirectory() + QStringLiteral("/ExportNamingEditor.qml")));
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///NamingEditorHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      (void)QTest::qWaitForWindowExposed(window);
      ProcessEvents();
      editor = window->findChild<QQuickItem*>(QStringLiteral("exportNamingEditor"));
    }
  }
};

TEST(ExportNamingEditorQmlTest, DefaultPatternLoadsWithSourcePreviewAndNoWarnings) {
  NamingEditorHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_NE(harness.editor, nullptr) << harness.warnings.join('\n').toStdString();
  EXPECT_TRUE(harness.editor->property("patternValid").toBool());
  EXPECT_EQ(harness.editor->property("pattern").toString(), QStringLiteral("{source}"));
  EXPECT_EQ(harness.editor->property("previewName").toString(), QStringLiteral("DSCF2074.jpg"));
  EXPECT_NE(harness.window->findChild<QQuickItem*>(QStringLiteral("exportFileNamePatternField")),
            nullptr);
  // File name presets moved to the export presets (ExportPresetPicker).
  EXPECT_EQ(harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingPresetCombo")),
            nullptr);
  EXPECT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
}

TEST(ExportNamingEditorQmlTest, PatternPreviewUpdatesForOrderedFieldsAndRejectsUnknownField) {
  NamingEditorHarness harness;
  ASSERT_NE(harness.editor, nullptr) << harness.warnings.join('\n').toStdString();

  ASSERT_TRUE(harness.editor->setProperty(
      "pattern", QStringLiteral("{date:yyyy MM dd}-{cameraModel}-{sequence:0000}")));
  ProcessEvents();
  EXPECT_TRUE(harness.editor->property("patternValid").toBool());
  EXPECT_EQ(harness.editor->property("previewName").toString(),
            QStringLiteral("2026 08 10-X-T5-0001.jpg"));

  ASSERT_TRUE(harness.editor->setProperty("pattern", QStringLiteral("{unknown}")));
  ProcessEvents();
  EXPECT_FALSE(harness.editor->property("patternValid").toBool());
  EXPECT_EQ(harness.editor->property("previewName").toString(),
            QStringLiteral("Invalid pattern.jpg"));
}

}  // namespace
}  // namespace alcedo::ui::test
