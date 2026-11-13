//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase A2: RegisteredShortcut.qml binds a window-context Shortcut to one
// ShortcutRegistry command. This harness loads the component from its source
// file directly (no application shell boot) and verifies scope gating,
// editable-focus suppression, live rebinding, and auto-repeat resolution.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <filesystem>

#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

constexpr int     kKeyChord   = static_cast<int>(ShortcutInputKind::KeyChord);

const char* const kHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import Alcedo.Main 1.0
import "__QML_DIR__"

ApplicationWindow {
    id: root
    objectName: "registeredShortcutHarness"
    width: 480
    height: 320
    visible: true

    property string testedCommand: "library.selectAll"
    property string testedScope: "workspace.library"
    property bool testedEnabled: true
    property bool testedSuppress: true
    property int activations: 0
    property int fallthroughPresses: 0
    property var keyLog: []

    RegisteredShortcut {
        id: testedShortcut
        objectName: "testedShortcut"
        commandId: root.testedCommand
        activeScope: root.testedScope
        commandEnabled: root.testedEnabled
        suppressWhileEditing: root.testedSuppress
        onActivated: root.activations += 1
    }

    Item {
        id: focusItem
        objectName: "focusItem"
        focus: true
        Keys.onPressed: function(event) {
            root.fallthroughPresses += 1
            root.keyLog = root.keyLog.concat(["press:" + event.key + ":" + event.modifiers])
        }
        Keys.onReleased: function(event) {
            root.keyLog = root.keyLog.concat(["release:" + event.key + ":" + event.modifiers])
        }
    }

    TextField {
        id: testTextField
        objectName: "testTextField"
        width: 240
        text: "sample text"
    }
}
)QML";

auto              QmlDirectoryUrl() -> QString {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

void ProcessEvents(int milliseconds = 30) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

class RegisteredShortcutHarness {
 public:
  RegisteredShortcutHarness() {
    // The QML singleton stores overrides in a default-constructed QSettings;
    // point it at an isolated INI directory so runs never touch real settings.
    if (!settings_dir_.isValid()) {
      warnings_.push_back(QStringLiteral("temporary settings directory unavailable"));
      return;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RegisteredShortcutHarness"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    RegisterShortcutRegistryQmlType();
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) {
        warnings_.push_back(warning.toString());
      }
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));

    QByteArray qml{kHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///RegisteredShortcutHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents();
  }

  ~RegisteredShortcutHarness() {
    // Never leak test bindings into the next harness instance's INI dir.
    auto* registry =
        engine_.singletonInstance<ShortcutRegistry*>("Alcedo.Main", "ShortcutRegistry");
    if (registry != nullptr) {
      for (const auto* command_id : {"library.selectAll", "filmstrip.nextImage"}) {
        registry->restoreDefault(QLatin1String(command_id));
      }
    }
  }

  [[nodiscard]] ShortcutRegistry* registry() {
    return engine_.singletonInstance<ShortcutRegistry*>("Alcedo.Main", "ShortcutRegistry");
  }

  void FocusItem() {
    auto* item = window_->findChild<QQuickItem*>(QStringLiteral("focusItem"));
    if (item != nullptr) {
      item->forceActiveFocus();
      ProcessEvents();
    }
  }

  void FocusTextField() {
    auto* field = window_->findChild<QQuickItem*>(QStringLiteral("testTextField"));
    if (field != nullptr) {
      field->forceActiveFocus();
      ProcessEvents();
    }
  }

  void ClickA() {
    QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
    ProcessEvents();
  }

  QTemporaryDir         settings_dir_;
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

auto ShortcutUnderTest(QQuickWindow* window) -> QQuickItem* {
  return window != nullptr ? window->findChild<QQuickItem*>(QStringLiteral("testedShortcut"))
                           : nullptr;
}

TEST(RegisteredShortcutQmlTest, ScopeMismatchKeepsShortcutInert) {
  RegisteredShortcutHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* shortcut = ShortcutUnderTest(harness.window_);
  ASSERT_NE(shortcut, nullptr);
  harness.FocusItem();

  EXPECT_TRUE(shortcut->property("shortcutEnabled").toBool());
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 1);

  harness.window_->setProperty("testedScope", QStringLiteral("workspace.editor"));
  ProcessEvents();
  EXPECT_FALSE(shortcut->property("shortcutEnabled").toBool());
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 1);

  harness.window_->setProperty("testedScope", QStringLiteral("workspace.library"));
  ProcessEvents();
  EXPECT_TRUE(shortcut->property("shortcutEnabled").toBool());
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 2);
}

TEST(RegisteredShortcutQmlTest, DisabledCommandPassesKeyThroughToFocusItem) {
  RegisteredShortcutHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* shortcut = ShortcutUnderTest(harness.window_);
  ASSERT_NE(shortcut, nullptr);
  harness.FocusItem();

  harness.window_->setProperty("testedEnabled", false);
  ProcessEvents();
  EXPECT_FALSE(shortcut->property("shortcutEnabled").toBool());

  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 0);
  // The unconsumed key reaches the focused item instead of being swallowed.
  EXPECT_GE(harness.window_->property("fallthroughPresses").toInt(), 1)
      << harness.window_->property("keyLog")
             .toStringList()
             .join(QStringLiteral(", "))
             .toStdString();
}

TEST(RegisteredShortcutQmlTest, EditableFocusSuppressesActivation) {
  RegisteredShortcutHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* shortcut = ShortcutUnderTest(harness.window_);
  auto* field    = harness.window_->findChild<QQuickItem*>(QStringLiteral("testTextField"));
  ASSERT_NE(shortcut, nullptr);
  ASSERT_NE(field, nullptr);
  harness.FocusTextField();

  // Suppressed: the command's enabled gate closes and the field keeps the
  // native Ctrl+A (select-all) behavior.
  EXPECT_FALSE(shortcut->property("shortcutEnabled").toBool());
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 0);
  EXPECT_EQ(field->property("selectedText").toString(), QStringLiteral("sample text"));

  // Suppression off: the enabled gate opens again; the command still fires
  // once a non-editable item owns focus.
  harness.window_->setProperty("testedSuppress", false);
  ProcessEvents();
  EXPECT_TRUE(shortcut->property("shortcutEnabled").toBool());
  harness.FocusItem();
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 1);
}

TEST(RegisteredShortcutQmlTest, BindingUpdateRetargetsLiveShortcut) {
  RegisteredShortcutHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* shortcut = ShortcutUnderTest(harness.window_);
  auto* registry = harness.registry();
  ASSERT_NE(shortcut, nullptr);
  ASSERT_NE(registry, nullptr);
  harness.FocusItem();

  EXPECT_EQ(shortcut->property("sequences").toStringList(), QStringList{QStringLiteral("Ctrl+A")});

  const auto result = registry->saveCandidate(QStringLiteral("library.selectAll"), Qt::Key_I,
                                              Qt::ControlModifier, kKeyChord);
  ASSERT_TRUE(result.value(QStringLiteral("succeeded")).toBool())
      << result.value(QStringLiteral("message")).toString().toStdString();
  ProcessEvents();

  EXPECT_EQ(shortcut->property("sequences").toStringList(), QStringList{QStringLiteral("Ctrl+I")});
  harness.ClickA();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 0);

  QTest::keyClick(harness.window_, Qt::Key_I, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(harness.window_->property("activations").toInt(), 1);

  const auto restored = registry->restoreDefault(QStringLiteral("library.selectAll"));
  ASSERT_TRUE(restored.value(QStringLiteral("succeeded")).toBool());
  ProcessEvents();
  EXPECT_EQ(shortcut->property("sequences").toStringList(), QStringList{QStringLiteral("Ctrl+A")});
}

TEST(RegisteredShortcutQmlTest, AutoRepeatFollowsCommandSpec) {
  RegisteredShortcutHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();

  auto* shortcut = ShortcutUnderTest(harness.window_);
  ASSERT_NE(shortcut, nullptr);

  // library.selectAll is a one-shot command; filmstrip.nextImage repeats.
  EXPECT_FALSE(shortcut->property("autoRepeat").toBool());

  harness.window_->setProperty("testedCommand", QStringLiteral("filmstrip.nextImage"));
  harness.window_->setProperty("testedScope", QStringLiteral("editor.filmstrip"));
  ProcessEvents();
  EXPECT_TRUE(shortcut->property("autoRepeat").toBool());
}

}  // namespace
}  // namespace alcedo::ui::test
