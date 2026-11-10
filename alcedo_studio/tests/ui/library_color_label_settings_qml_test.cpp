//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > Library > Color Labels (issue 770) on the production SettingDialog and
// ApplicationModuleHost. QSettings goes to a temporary INI directory, so a "restart" is a new
// LibraryViewPreferences that reads the same directory. Text is typed with key events into the
// focused field; the reset glyph is activated through its `clicked` signal (offscreen pointer
// delivery into the dialog's scrolled page is not reliable, AGENTS.md).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QInputMethodEvent>
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
#include <memory>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

void ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

const char* const kDialogHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: 1280
    height: 800
    visible: true

    SettingDialog {
        objectName: "settingsDialog"
        languageOptions: languageManager.availableLanguages
    }
}
)QML";

void              IsolateSettings(const QString& root) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  QCoreApplication::setApplicationName(QStringLiteral("LibraryColorLabelSettingsHarness"));
}

/// The settings directory outlives the harness, so a second harness or a new
/// LibraryViewPreferences reads what the first one stored.
class SettingsDirectory {
 public:
  SettingsDirectory() { IsolateSettings(dir_.path()); }
  [[nodiscard]] auto valid() const -> bool { return dir_.isValid(); }

 private:
  QTemporaryDir dir_;
};

class SettingsDialogHarness {
 public:
  SettingsDialogHarness() {
    AppTheme::RegisterFonts();
    // The style can be set once per process, before the first Controls import.
    static const bool style_set = [] {
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      return true;
    }();
    Q_UNUSED(style_set);
    host_             = std::make_unique<ApplicationModuleHost>();
    language_manager_ = std::make_unique<LanguageManager>(QCoreApplication::instance());
    AppTheme::SetEffectiveLanguageCode(language_manager_->EffectiveLanguageCode());
    RegisterShortcutRegistryQmlType();
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), host_.get());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                              language_manager_.get());
    host_->AttachQmlEngine(&engine_);

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///LibraryColorLabelSettingsHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents(120);
  }

  auto dialog() -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(QStringLiteral("settingsDialog"))
                              : nullptr;
  }
  auto item(const QString& name) -> QQuickItem* {
    return window_ != nullptr ? window_->findChild<QQuickItem*>(name) : nullptr;
  }
  auto input(int code) -> QQuickItem* {
    return item(QStringLiteral("colorLabelNameInput:%1").arg(code));
  }
  auto reset(int code) -> QQuickItem* {
    return item(QStringLiteral("colorLabelNameReset:%1").arg(code));
  }
  auto placeholder(int code) -> QQuickItem* {
    auto* field = input(code);
    return field != nullptr
               ? field->findChild<QQuickItem*>(QStringLiteral("colorLabelNamePlaceholder"))
               : nullptr;
  }
  auto preferences() -> LibraryViewPreferences* { return host_->library_view(); }

  void Open() {
    dialog()->setProperty("requestedCategory", 0);
    QMetaObject::invokeMethod(dialog(), "open");
    ProcessEvents(150);
  }
  void Apply() {
    QMetaObject::invokeMethod(dialog(), "applySettings");
    ProcessEvents(150);
  }

  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<LanguageManager>       language_manager_;
  QQmlApplicationEngine                  engine_;
  QQuickWindow*                          window_ = nullptr;
  QStringList                            warnings_;
};

auto Text(QQuickItem* item) -> QString {
  return item != nullptr ? item->property("text").toString() : QString();
}

/// Types @p text into the focused item of @p window, one key event per character.
void TypeText(QQuickWindow* window, const QString& text) {
  for (const QChar character : text) {
    QTest::keyClick(window, character.toLatin1());
  }
}

TEST(LibraryColorLabelSettingsQmlTest, NameTypedInSettingsIsAppliedAndSurvivesARestart) {
  SettingsDirectory settings;
  ASSERT_TRUE(settings.valid());
  {
    SettingsDialogHarness harness;
    ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
    harness.Open();

    // Seven rows; every field is empty with the color name as placeholder, without reset glyph.
    const QStringList defaults = harness.preferences()->color_label_default_names();
    ASSERT_EQ(defaults.size(), 7);
    for (int code = 0; code < 7; ++code) {
      auto* field = harness.input(code);
      ASSERT_NE(field, nullptr) << code;
      EXPECT_TRUE(Text(field).isEmpty()) << code;
      ASSERT_NE(harness.placeholder(code), nullptr) << code;
      EXPECT_EQ(Text(harness.placeholder(code)), defaults.at(code)) << code;
      EXPECT_TRUE(harness.placeholder(code)->isVisible()) << code;
      ASSERT_NE(harness.reset(code), nullptr) << code;
      EXPECT_FALSE(harness.reset(code)->isVisible()) << code;
    }
    EXPECT_EQ(harness.input(7), nullptr);

    auto* field = harness.input(0);
    field->forceActiveFocus();
    ProcessEvents();
    ASSERT_TRUE(field->hasActiveFocus());
    TypeText(harness.window_, QStringLiteral("Client picks"));
    ProcessEvents();
    EXPECT_EQ(Text(field), QStringLiteral("Client picks"));
    EXPECT_FALSE(harness.placeholder(0)->isVisible());
    EXPECT_TRUE(harness.reset(0)->isVisible());
    // The name waits for Apply, like the other settings of the page.
    EXPECT_EQ(harness.preferences()->color_label_names().at(0), defaults.at(0));

    harness.Apply();
    EXPECT_EQ(harness.preferences()->color_label_names().at(0), QStringLiteral("Client picks"));
    EXPECT_EQ(harness.preferences()->color_label_names().mid(1), defaults.mid(1));
    EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
  }

  LibraryViewPreferences restarted;
  EXPECT_EQ(restarted.color_label_names().at(0), QStringLiteral("Client picks"));

  // The next Settings opening shows the stored name in the field.
  SettingsDialogHarness reopened;
  ASSERT_NE(reopened.dialog(), nullptr) << reopened.warnings_.join('\n').toStdString();
  reopened.Open();
  EXPECT_EQ(Text(reopened.input(0)), QStringLiteral("Client picks"));
  EXPECT_TRUE(reopened.reset(0)->isVisible());
}

TEST(LibraryColorLabelSettingsQmlTest, ResetGlyphAndEmptyNameReturnTheColorName) {
  SettingsDirectory settings;
  ASSERT_TRUE(settings.valid());
  {
    QSettings stored;
    stored.setValue(LibraryViewPreferences::ColorLabelNameSettingKey(3),
                    QStringLiteral("Portfolio"));
    stored.setValue(LibraryViewPreferences::ColorLabelNameSettingKey(4),
                    QStringLiteral("Print 30x40"));
  }

  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open();
  const QStringList defaults = harness.preferences()->color_label_default_names();

  EXPECT_EQ(Text(harness.input(3)), QStringLiteral("Portfolio"));
  EXPECT_EQ(Text(harness.input(4)), QStringLiteral("Print 30x40"));
  ASSERT_NE(harness.reset(3), nullptr);
  ASSERT_TRUE(harness.reset(3)->isVisible());
  EXPECT_TRUE(harness.reset(4)->isVisible());

  // The reset glyph empties the field and shows the color name again.
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.reset(3), "clicked"));
  ProcessEvents();
  EXPECT_TRUE(Text(harness.input(3)).isEmpty());
  EXPECT_TRUE(harness.placeholder(3)->isVisible());
  EXPECT_EQ(Text(harness.placeholder(3)), defaults.at(3));
  EXPECT_FALSE(harness.reset(3)->isVisible());

  // Deleting every character of a name is the same as the reset glyph.
  auto* field = harness.input(4);
  field->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(field->hasActiveFocus());
  QTest::keyClick(harness.window_, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClick(harness.window_, Qt::Key_Backspace);
  ProcessEvents();
  EXPECT_TRUE(Text(field).isEmpty());
  EXPECT_FALSE(harness.reset(4)->isVisible());

  harness.Apply();
  EXPECT_EQ(harness.preferences()->color_label_names(), defaults);
  EXPECT_FALSE(QSettings{}.contains(LibraryViewPreferences::ColorLabelNameSettingKey(3)));
  EXPECT_FALSE(QSettings{}.contains(LibraryViewPreferences::ColorLabelNameSettingKey(4)));

  LibraryViewPreferences restarted;
  EXPECT_EQ(restarted.color_label_names(), defaults);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(LibraryColorLabelSettingsQmlTest, ClosingWithoutApplyKeepsTheStoredNames) {
  SettingsDirectory settings;
  ASSERT_TRUE(settings.valid());

  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open();
  const QStringList defaults = harness.preferences()->color_label_default_names();

  auto*             field    = harness.input(2);
  ASSERT_NE(field, nullptr);
  field->forceActiveFocus();
  ProcessEvents();
  TypeText(harness.window_, QStringLiteral("Draft"));
  ProcessEvents();
  EXPECT_EQ(Text(field), QStringLiteral("Draft"));

  QMetaObject::invokeMethod(harness.dialog(), "close");
  ProcessEvents(150);
  EXPECT_EQ(harness.preferences()->color_label_names(), defaults);

  // The next opening starts from the stored names.
  harness.Open();
  EXPECT_TRUE(Text(harness.input(2)).isEmpty());
  EXPECT_FALSE(harness.reset(2)->isVisible());
}

/// Commits @p text into @p field at its cursor, the path of an input method and of a paste.
void CommitText(QQuickItem* field, const QString& text) {
  QInputMethodEvent event;
  event.setCommitString(text);
  QCoreApplication::sendEvent(field, &event);
  ProcessEvents();
}

// Issue 780: the field applies the rule of LibraryViewPreferences while the user types: 40
// characters with surrogate pairs whole, no control characters, line breaks as spaces.
TEST(LibraryColorLabelSettingsQmlTest, FieldKeepsWholeCharactersAndDropsControlCharacters) {
  SettingsDirectory settings;
  ASSERT_TRUE(settings.valid());
  const QString         emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
  const QString         x39(39, QLatin1Char('x'));

  SettingsDialogHarness harness;
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings_.join('\n').toStdString();
  harness.Open();

  auto* blue = harness.input(4);
  ASSERT_NE(blue, nullptr);
  blue->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(blue->hasActiveFocus());
  TypeText(harness.window_, x39);
  CommitText(blue, emoji + QStringLiteral("tail"));
  EXPECT_EQ(Text(blue), x39 + emoji);
  EXPECT_EQ(blue->property("cursorPosition").toInt(), 41);

  auto* red = harness.input(0);
  red->forceActiveFocus();
  ProcessEvents();
  CommitText(red, QStringLiteral("Client\npicks"));
  EXPECT_EQ(Text(red), QStringLiteral("Client picks"));
  // A control character in the middle goes; the cursor stays after the typed text.
  red->setProperty("cursorPosition", 6);
  CommitText(red, QString(QChar(0x202E)) + QStringLiteral("s"));
  EXPECT_EQ(Text(red), QStringLiteral("Clients picks"));
  EXPECT_EQ(red->property("cursorPosition").toInt(), 7);
  // The field still follows the pending name: typing goes on, and the reset glyph empties it.
  TypeText(harness.window_, QStringLiteral("!"));
  EXPECT_EQ(Text(red), QStringLiteral("Clients! picks"));

  harness.Apply();
  EXPECT_EQ(harness.preferences()->color_label_names().at(4), x39 + emoji);
  EXPECT_EQ(harness.preferences()->color_label_names().at(0), QStringLiteral("Clients! picks"));
  EXPECT_EQ(QSettings{}.value(LibraryViewPreferences::ColorLabelNameSettingKey(4)).toString(),
            x39 + emoji);

  ASSERT_TRUE(QMetaObject::invokeMethod(harness.reset(0), "clicked"));
  ProcessEvents();
  EXPECT_TRUE(Text(red).isEmpty());
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
