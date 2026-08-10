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
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>

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
    property string savedPresetName: ""
    property string savedPresetPattern: ""
    property string replacedPresetName: ""
    property string deletedPresetName: ""

    Loader {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        source: editorSourceUrl
        onLoaded: {
            item.objectName = "exportNamingEditor"
            item.sampleSourceName = "DSCF2074.RAF"
            item.outputExtension = ".jpg"
            item.savePreset = function(name, pattern, replacedName) {
                harnessRoot.savedPresetName = name
                harnessRoot.savedPresetPattern = pattern
                harnessRoot.replacedPresetName = replacedName
                return true
            }
            item.deletePreset = function(name) {
                harnessRoot.deletedPresetName = name
                return true
            }
        }
    }
}
)";

struct NamingEditorHarness {
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
  const auto* preset_combo =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingPresetCombo"));
  ASSERT_NE(preset_combo, nullptr);
  EXPECT_EQ(preset_combo->property("currentIndex").toInt(), 0);
  const auto* save_button =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingSavePresetButton"));
  ASSERT_NE(save_button, nullptr);
  EXPECT_FALSE(save_button->isEnabled());
  const auto* delete_button =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingDeletePresetButton"));
  ASSERT_NE(delete_button, nullptr);
  EXPECT_FALSE(delete_button->isEnabled());
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
  const auto* preset_combo =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingPresetCombo"));
  ASSERT_NE(preset_combo, nullptr);
  EXPECT_EQ(preset_combo->property("currentIndex").toInt(), 3);
  const auto* save_button =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingSavePresetButton"));
  ASSERT_NE(save_button, nullptr);
  EXPECT_TRUE(save_button->isEnabled());
  const auto* delete_button =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingDeletePresetButton"));
  ASSERT_NE(delete_button, nullptr);
  EXPECT_FALSE(delete_button->isEnabled());

  ASSERT_TRUE(harness.editor->setProperty("pattern", QStringLiteral("{unknown}")));
  ProcessEvents();
  EXPECT_FALSE(harness.editor->property("patternValid").toBool());
  EXPECT_EQ(harness.editor->property("previewName").toString(),
            QStringLiteral("Invalid pattern.jpg"));
}

TEST(ExportNamingEditorQmlTest, SavedPresetCanBeSelectedAndCurrentPatternCanBeNamed) {
  NamingEditorHarness harness;
  ASSERT_NE(harness.editor, nullptr) << harness.warnings.join('\n').toStdString();

  const QVariantList saved_presets{
      QVariantMap{{QStringLiteral("name"), QStringLiteral("Client")},
                  {QStringLiteral("pattern"), QStringLiteral("Client-{sequence:000}")}}};
  ASSERT_TRUE(harness.editor->setProperty("savedPresets", saved_presets));
  ASSERT_TRUE(harness.editor->setProperty("pattern", QStringLiteral("Client-{sequence:000}")));
  ProcessEvents();

  const auto* preset_combo =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingPresetCombo"));
  ASSERT_NE(preset_combo, nullptr);
  EXPECT_EQ(preset_combo->property("currentIndex").toInt(), 3);
  auto* delete_button =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingDeletePresetButton"));
  ASSERT_NE(delete_button, nullptr);
  EXPECT_TRUE(delete_button->isEnabled());

  ASSERT_TRUE(harness.editor->setProperty("pattern", QStringLiteral("Client-v2-{sequence:000}")));
  ProcessEvents();
  EXPECT_EQ(preset_combo->property("currentIndex").toInt(), 4);
  EXPECT_FALSE(delete_button->isEnabled());
  EXPECT_EQ(harness.editor->property("selectedSavedPresetName").toString(),
            QStringLiteral("Client"));
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.editor, "beginPresetNaming"));
  ProcessEvents();
  EXPECT_TRUE(harness.editor->property("namingPreset").toBool());

  auto* name_field =
      harness.window->findChild<QQuickItem*>(QStringLiteral("exportNamingPresetNameField"));
  ASSERT_NE(name_field, nullptr);
  EXPECT_EQ(name_field->property("text").toString(), QStringLiteral("Client"));
  ASSERT_TRUE(name_field->setProperty("text", QStringLiteral("Archive")));
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.editor, "saveNamedPreset"));
  ProcessEvents();

  EXPECT_EQ(harness.window->property("savedPresetName").toString(), QStringLiteral("Archive"));
  EXPECT_EQ(harness.window->property("savedPresetPattern").toString(),
            QStringLiteral("Client-v2-{sequence:000}"));
  EXPECT_EQ(harness.window->property("replacedPresetName").toString(), QStringLiteral("Client"));
  EXPECT_FALSE(harness.editor->property("namingPreset").toBool());

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.editor, "deleteSavedPreset",
                                        Q_ARG(QVariant, QStringLiteral("Archive"))));
  EXPECT_EQ(harness.window->property("deletedPresetName").toString(), QStringLiteral("Archive"));
  EXPECT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
