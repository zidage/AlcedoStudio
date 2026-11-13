//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ExportPresetPicker.qml with the presets of ExportRecipeBuilder: the preset that matches the
// panel options, applying a preset, and saving and deleting a user preset.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
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
    height: 300
    visible: true
    color: appTheme.bgCanvasColor
    property var panelOptions: exportRecipe.DefaultOptions()
    property int selectedCount: 0

    Loader {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        source: pickerSourceUrl
        onLoaded: {
            item.objectName = "exportPresetPicker"
            item.recipe = exportRecipe
            item.currentOptions = Qt.binding(function() { return harnessRoot.panelOptions })
            item.presetSelected.connect(function(options) {
                harnessRoot.selectedCount += 1
                harnessRoot.panelOptions = options
            })
        }
    }
}
)";

struct PresetPickerHarness {
  ExportRecipeBuilder   export_recipe{nullptr};
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           picker = nullptr;
  QStringList           warnings;

  PresetPickerHarness() {
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& messages) {
      for (const auto& message : messages) warnings.push_back(message.toString());
    });
    engine.addImportPath(SourceQmlDirectory());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("exportRecipe"), &export_recipe);
    engine.rootContext()->setContextProperty(
        QStringLiteral("pickerSourceUrl"),
        QUrl::fromLocalFile(SourceQmlDirectory() + QStringLiteral("/ExportPresetPicker.qml")));
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///PresetPickerHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      (void)QTest::qWaitForWindowExposed(window);
      ProcessEvents();
      picker = window->findChild<QQuickItem*>(QStringLiteral("exportPresetPicker"));
    }
  }

  auto PanelOptions() const -> QVariantMap {
    return window->property("panelOptions").toMap();
  }

  void SetPanelOptions(const QVariantMap& options) {
    window->setProperty("panelOptions", options);
    ProcessEvents();
  }

  auto Button(const char* object_name) const -> QQuickItem* {
    return window->findChild<QQuickItem*>(QString::fromLatin1(object_name));
  }
};

/// QSettings in an INI file of a temporary folder, so the tests do not touch the user presets.
class ExportPresetPickerQmlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ExportPresetPickerQmlTest"));
    QSettings{}.clear();
  }

  void TearDown() override { QSettings{}.clear(); }

  QTemporaryDir settings_dir_;
};

TEST_F(ExportPresetPickerQmlTest, DefaultOptionsShowStandardPresetWithoutCustomEntry) {
  PresetPickerHarness harness;
  ASSERT_NE(harness.picker, nullptr) << harness.warnings.join('\n').toStdString();

  EXPECT_EQ(harness.picker->property("activePresetId").toString(),
            QStringLiteral("builtin:standard"));
  const QVariantList entries = harness.picker->property("presetEntries").toList();
  ASSERT_EQ(entries.size(), 7);
  EXPECT_EQ(entries[1].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Web"));
  const auto* combo = harness.Button("exportPresetCombo");
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->property("currentIndex").toInt(), 0);
  ASSERT_NE(harness.Button("exportSavePresetButton"), nullptr);
  EXPECT_FALSE(harness.Button("exportSavePresetButton")->isEnabled());
  ASSERT_NE(harness.Button("exportDeletePresetButton"), nullptr);
  EXPECT_FALSE(harness.Button("exportDeletePresetButton")->isEnabled());
  EXPECT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
}

TEST_F(ExportPresetPickerQmlTest, SelectingPresetSendsItsOptionsAndKeepsTheDestination) {
  PresetPickerHarness harness;
  ASSERT_NE(harness.picker, nullptr) << harness.warnings.join('\n').toStdString();
  QVariantMap options = harness.PanelOptions();
  options.insert(QStringLiteral("out_dir"), QStringLiteral("D:/Exports"));
  harness.SetPanelOptions(options);

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.picker, "selectPreset",
                                        Q_ARG(QVariant, QStringLiteral("builtin:web"))));
  ProcessEvents();
  EXPECT_EQ(harness.window->property("selectedCount").toInt(), 1);
  const QVariantMap applied = harness.PanelOptions();
  EXPECT_EQ(applied.value(QStringLiteral("resize_mode")).toString(), QStringLiteral("long_edge"));
  EXPECT_EQ(applied.value(QStringLiteral("long_edge")).toInt(), 2048);
  EXPECT_EQ(applied.value(QStringLiteral("quality")).toInt(), 80);
  EXPECT_FALSE(applied.value(QStringLiteral("include_gps")).toBool());
  EXPECT_EQ(applied.value(QStringLiteral("ultra_hdr_long_edge")).toInt(), 2048);
  EXPECT_EQ(applied.value(QStringLiteral("out_dir")).toString(), QStringLiteral("D:/Exports"));
  EXPECT_EQ(harness.picker->property("activePresetId").toString(), QStringLiteral("builtin:web"));
  EXPECT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
}

TEST_F(ExportPresetPickerQmlTest, CustomOptionsCanBeSavedAsUserPresetAndDeleted) {
  PresetPickerHarness harness;
  ASSERT_NE(harness.picker, nullptr) << harness.warnings.join('\n').toStdString();
  QVariantMap options = harness.PanelOptions();
  options.insert(QStringLiteral("quality"), 61);
  options.insert(QStringLiteral("include_gps"), false);
  harness.SetPanelOptions(options);

  EXPECT_TRUE(harness.picker->property("customOptions").toBool());
  QVariantList entries = harness.picker->property("presetEntries").toList();
  ASSERT_EQ(entries.size(), 8);
  EXPECT_EQ(entries.back().toMap().value(QStringLiteral("label")).toString(),
            QStringLiteral("Custom"));
  ASSERT_TRUE(harness.Button("exportSavePresetButton")->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.picker, "beginPresetNaming"));
  ProcessEvents();
  EXPECT_TRUE(harness.picker->property("namingPreset").toBool());
  auto* name_field = harness.Button("exportPresetNameField");
  ASSERT_NE(name_field, nullptr);
  ASSERT_TRUE(name_field->setProperty("text", QStringLiteral("Proofs")));
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.picker, "saveNamedPreset"));
  ProcessEvents();

  EXPECT_FALSE(harness.picker->property("namingPreset").toBool());
  ASSERT_EQ(ExportRecipeBuilder::UserPresets().size(), 1);
  EXPECT_EQ(ExportRecipeBuilder::UserPresets()[0].options.quality, 61);
  EXPECT_EQ(harness.picker->property("activePresetId").toString(), QStringLiteral("user:Proofs"));
  entries = harness.picker->property("presetEntries").toList();
  ASSERT_EQ(entries.size(), 8);
  EXPECT_EQ(entries.back().toMap().value(QStringLiteral("label")).toString(),
            QStringLiteral("Proofs"));
  EXPECT_TRUE(harness.Button("exportDeletePresetButton")->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.picker, "deletePreset",
                                        Q_ARG(QVariant, QStringLiteral("user:Proofs"))));
  ProcessEvents();
  EXPECT_TRUE(ExportRecipeBuilder::UserPresets().isEmpty());
  EXPECT_TRUE(harness.picker->property("customOptions").toBool());
  EXPECT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
