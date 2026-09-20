//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase B1: Settings > Keyboard page and ShortcutCaptureField interaction.
//
// Two harnesses load production QML straight from the source directory:
//   - ShortcutPanelHarness embeds KeyboardSettingsPanel next to a live
//     RegisteredShortcut probe (live rebinding evidence).
//   - SettingsDialogHarness hosts the production SettingDialog on a real
//     ApplicationModuleHost (category navigation, dialog Escape policy).
// Every harness redirects QSettings into an isolated INI directory so capture
// commits never touch real application settings.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <filesystem>
#include <fstream>
#include <memory>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
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

auto QmlDirectoryPath() -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
}

// Translated strings go through the same installed translator the engine saw
// at load time, so assertions stay correct under a non-English OS locale.
auto RegistryText(const char* source) -> QString {
  return QCoreApplication::translate("ShortcutRegistry", source);
}
auto FieldText(const char* source) -> QString {
  return QCoreApplication::translate("ShortcutCaptureField", source);
}
auto DialogText(const char* source) -> QString {
  return QCoreApplication::translate("SettingDialog", source);
}

// ---------------------------------------------------------------------------
// Panel harness: KeyboardSettingsPanel + live RegisteredShortcut probe.
// ---------------------------------------------------------------------------

const char* const kPanelHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import Alcedo.Main 1.0
import "__QML_DIR__"

ApplicationWindow {
    id: root
    objectName: "shortcutSettingsHarness"
    width: 960
    height: 640
    visible: true

    property int probeActivations: 0
    property string probeCommandId: "library.selectAll"
    property string probeScope: "workspace.library"

    ScrollView {
        id: scroll
        objectName: "settingsScroll"
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        KeyboardSettingsPanel {
            id: keyboardPanel
            objectName: "keyboardSettingsPanel"
            width: scroll.availableWidth
        }
    }

    RegisteredShortcut {
        id: probeShortcut
        objectName: "probeShortcut"
        commandId: root.probeCommandId
        activeScope: root.probeScope
        onActivated: root.probeActivations += 1
    }
}
)QML";

// ---------------------------------------------------------------------------
// Dialog harness: the production SettingDialog on a real module host.
// ---------------------------------------------------------------------------

const char* const kDialogHarnessQml = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    id: root
    objectName: "settingsDialogHarness"
    width: 1280
    height: 800
    visible: true

    SettingDialog {
        id: settings
        objectName: "settingsDialog"
        languageOptions: languageManager.availableLanguages
    }
}
)QML";

void IsolateSettings(const QString& root) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  QCoreApplication::setApplicationName(QStringLiteral("ShortcutSettingsHarness"));
}

void WatchEngineWarnings(QQmlApplicationEngine& engine, QStringList& warnings) {
  QObject::connect(&engine, &QQmlEngine::warnings, [&warnings](const QList<QQmlError>& emitted) {
    for (const auto& warning : emitted) {
      warnings.push_back(warning.toString());
    }
  });
}

class ShortcutPanelHarness {
 public:
  explicit ShortcutPanelHarness(const QString& settings_root = QString()) {
    const QString root = settings_root.isEmpty() ? settings_dir_.path() : settings_root;
    if (root.isEmpty()) {
      warnings_.push_back(QStringLiteral("temporary settings directory unavailable"));
      return;
    }
    IsolateSettings(root);

    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    RegisterShortcutRegistryQmlType();
    WatchEngineWarnings(engine_, warnings_);
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                            &AppTheme::Instance());

    QByteArray qml{kPanelHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///ShortcutSettingsPanelHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents();
  }

  auto registry() -> ShortcutRegistry* {
    return engine_.singletonInstance<ShortcutRegistry*>("Alcedo.Main", "ShortcutRegistry");
  }

  QTemporaryDir         settings_dir_;
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

class SettingsDialogHarness {
 public:
  explicit SettingsDialogHarness(const QString& settings_root = QString()) {
    const QString root = settings_root.isEmpty() ? settings_dir_.path() : settings_root;
    if (root.isEmpty()) {
      warnings_.push_back(QStringLiteral("temporary settings directory unavailable"));
      return;
    }
    IsolateSettings(root);

    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // Host construction registers the Alcedo.Main C++ types the dialog pages
    // need (and, through the node-module registration, the ShortcutRegistry
    // singleton factory). The explicit call below is idempotent.
    host_             = std::make_unique<ApplicationModuleHost>();
    language_manager_ = std::make_unique<LanguageManager>(QCoreApplication::instance());
    AppTheme::SetEffectiveLanguageCode(language_manager_->EffectiveLanguageCode());
    RegisterShortcutRegistryQmlType();
    WatchEngineWarnings(engine_, warnings_);
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), host_.get());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                              &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                              language_manager_.get());
    host_->AttachQmlEngine(&engine_);

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///SettingsDialogHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
      if (window_ != nullptr) {
        window_->show();
        window_->requestActivate();
      }
    }
    ProcessEvents(120);
  }

  auto registry() -> ShortcutRegistry* {
    return engine_.singletonInstance<ShortcutRegistry*>("Alcedo.Main", "ShortcutRegistry");
  }
  // Dialog/Popup roots are QObject (not QQuickItem) — find them as objects.
  auto dialog() -> QObject* {
    return window_ != nullptr
               ? window_->findChild<QObject*>(QStringLiteral("settingsDialog"))
               : nullptr;
  }
  void OpenAt(int category) {
    auto* settings = dialog();
    if (settings == nullptr) {
      return;
    }
    settings->setProperty("requestedCategory", category);
    QMetaObject::invokeMethod(settings, "open");
    ProcessEvents(150);
  }

  QTemporaryDir                            settings_dir_;
  std::unique_ptr<ApplicationModuleHost>   host_;
  std::unique_ptr<LanguageManager>         language_manager_;
  QQmlApplicationEngine                    engine_;
  QQuickWindow*                            window_ = nullptr;
  QStringList                              warnings_;
};

// ---------------------------------------------------------------------------
// Shared finders / input helpers.
// ---------------------------------------------------------------------------

auto FindItem(QObject* root, const QString& name) -> QQuickItem* {
  return root != nullptr ? root->findChild<QQuickItem*>(name) : nullptr;
}

auto FieldForCommand(QObject* root, const QString& command_id) -> QQuickItem* {
  return FindItem(root, QStringLiteral("shortcutCaptureField:") + command_id);
}

void ScrollItemIntoView(QQuickWindow* window, const QString& scroll_name, QQuickItem* item) {
  auto* scroll = FindItem(window, scroll_name);
  ASSERT_NE(scroll, nullptr);
  ASSERT_NE(item, nullptr);
  // ScrollView (Pane) owns no contentY — the real scrollable is its inner
  // Flickable. contentItem may return the flickable or the pane's content
  // holder depending on version, so walk up to the Flickable either way.
  auto* content = scroll->property("contentItem").value<QQuickItem*>();
  ASSERT_NE(content, nullptr);
  auto* flickable = content;
  while (flickable != nullptr &&
         !QString::fromLatin1(flickable->metaObject()->className())
              .contains(QLatin1String("Flickable"))) {
    flickable = flickable->parentItem();
  }
  ASSERT_NE(flickable, nullptr);
  // Item position inside the scrolled content (contentY-independent).
  const QPointF pos = item->mapToItem(content, QPointF(0, 0));
  const qreal layout_y =
      content == flickable ? pos.y() + flickable->property("contentY").toReal() : pos.y();
  const qreal max_y =
      qMax<qreal>(0.0, flickable->property("contentHeight").toReal() -
                           flickable->property("height").toReal());
  flickable->setProperty("contentY", qBound<qreal>(0.0, layout_y - 24.0, max_y));
  ProcessEvents();
}

void ClickItem(QQuickWindow* window, QQuickItem* item) {
  ASSERT_NE(item, nullptr);
  const QPointF center_f =
      item->mapToScene(QPointF(item->width() * 0.5, item->height() * 0.5));
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center_f.toPoint());
  ProcessEvents();
}

// Clicks the field inside the panel-harness scroll and returns it capturing.
auto StartCapture(ShortcutPanelHarness& harness, const QString& command_id) -> QQuickItem* {
  auto* field = FieldForCommand(harness.window_, command_id);
  EXPECT_NE(field, nullptr);
  if (field == nullptr) {
    return nullptr;
  }
  ScrollItemIntoView(harness.window_, QStringLiteral("settingsScroll"), field);
  ClickItem(harness.window_, field);
  EXPECT_TRUE(field->property("capturing").toBool());
  return field;
}

auto StartDialogCapture(SettingsDialogHarness& harness, const QString& command_id)
    -> QQuickItem* {
  auto* field = FieldForCommand(harness.window_, command_id);
  EXPECT_NE(field, nullptr);
  if (field == nullptr) {
    return nullptr;
  }
  ScrollItemIntoView(harness.window_, QStringLiteral("keyboardSettingsScroll"), field);
  ClickItem(harness.window_, field);
  EXPECT_TRUE(field->property("capturing").toBool());
  return field;
}

// ---------------------------------------------------------------------------
// Navigation and model coverage.
// ---------------------------------------------------------------------------

TEST(ShortcutSettingsQmlTest, KeyboardCategoryIsReachableAndAboutStillOpens) {
  SettingsDialogHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* settings = harness.dialog();
  ASSERT_NE(settings, nullptr);
  harness.OpenAt(0);
  ASSERT_TRUE(settings->property("visible").toBool());

  // Reach Keyboard through the real category nav, not a direct index write.
  auto* nav_keyboard = FindItem(settings, QStringLiteral("settingsNavItem:7"));
  ASSERT_NE(nav_keyboard, nullptr);
  ClickItem(harness.window_, nav_keyboard);
  EXPECT_EQ(settings->property("currentCategory").toInt(), 7);

  auto* page_title = FindItem(settings, QStringLiteral("settingsPageTitle"));
  ASSERT_NE(page_title, nullptr);
  EXPECT_EQ(page_title->property("text").toString(), DialogText("Keyboard"));

  auto* keyboard_scroll = FindItem(settings, QStringLiteral("keyboardSettingsScroll"));
  auto* about_scroll    = FindItem(settings, QStringLiteral("aboutScroll"));
  auto* panel           = FindItem(settings, QStringLiteral("keyboardSettingsPanel"));
  ASSERT_NE(keyboard_scroll, nullptr);
  ASSERT_NE(about_scroll, nullptr);
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(keyboard_scroll->property("visible").toBool());
  EXPECT_FALSE(about_scroll->property("visible").toBool());
  EXPECT_NE(FieldForCommand(settings, QStringLiteral("library.selectAll")), nullptr);

  // About moved to index 8 and still opens.
  auto* nav_about = FindItem(settings, QStringLiteral("settingsNavItem:8"));
  ASSERT_NE(nav_about, nullptr);
  ClickItem(harness.window_, nav_about);
  EXPECT_EQ(settings->property("currentCategory").toInt(), 8);
  EXPECT_EQ(page_title->property("text").toString(), DialogText("About"));
  EXPECT_TRUE(about_scroll->property("visible").toBool());
  EXPECT_FALSE(keyboard_scroll->property("visible").toBool());
  EXPECT_NE(FindItem(settings, QStringLiteral("aboutVersionLabel")), nullptr);

  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(ShortcutSettingsQmlTest, RegistryRowsAppearInStableGroups) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);
  const int rows = registry->rowCount();
  ASSERT_GT(rows, 0);

  // Model truth: visible command order and the group label that opens each
  // contiguous run.
  QStringList expected_commands;
  QStringList expected_groups;
  for (int row = 0; row < rows; ++row) {
    const auto index = registry->index(row, 0);
    if (!registry->data(index, ShortcutRegistry::SettingsVisibleRole).toBool()) {
      continue;
    }
    expected_commands.push_back(
        registry->data(index, ShortcutRegistry::CommandIdRole).toString());
    const QString group = registry->data(index, ShortcutRegistry::GroupTextRole).toString();
    if (expected_groups.empty() || expected_groups.back() != group) {
      expected_groups.push_back(group);
    }
  }
  ASSERT_FALSE(expected_commands.empty());
  // The registered catalog starts with the known group sequence.
  EXPECT_EQ(expected_groups,
            (QStringList{RegistryText("Library"), RegistryText("Editor"),
                         RegistryText("Filmstrip"), RegistryText("Versions"),
                         RegistryText("LUT"), RegistryText("Masks"),
                         RegistryText("Nodes")}));

  const auto headers =
      harness.window_->findChildren<QQuickItem*>(QStringLiteral("shortcutGroupHeader"));
  QStringList rendered_groups;
  for (const auto* header : headers) {
    // Every row carries a header container; only the group-start row's is
    // visible. Count the effectively-visible ones (walk ancestors).
    bool effective_visible = true;
    for (auto* ancestor = header->parentItem(); ancestor != nullptr;
         ancestor = ancestor->parentItem()) {
      if (!ancestor->isVisible()) {
        effective_visible = false;
        break;
      }
    }
    if (effective_visible) {
      rendered_groups.push_back(header->property("groupKey").toString());
    }
  }
  EXPECT_EQ(rendered_groups, expected_groups);

  QStringList rendered_commands;
  const auto items = harness.window_->findChildren<QQuickItem*>();
  for (const auto* item : items) {
    const QString name = item->objectName();
    if (!name.startsWith(QLatin1String("shortcutCaptureField:"))) {
      continue;
    }
    bool effective_visible = true;
    for (auto* ancestor = item->parentItem(); ancestor != nullptr;
         ancestor = ancestor->parentItem()) {
      if (!ancestor->isVisible()) {
        effective_visible = false;
        break;
      }
    }
    if (effective_visible) {
      rendered_commands.push_back(
          name.mid(static_cast<int>(QLatin1String("shortcutCaptureField:").size())));
    }
  }
  EXPECT_EQ(rendered_commands, expected_commands);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// ---------------------------------------------------------------------------
// Capture interaction.
// ---------------------------------------------------------------------------

TEST(ShortcutSettingsQmlTest, ClickingBindingFieldStartsCapture) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("capturing"));
  EXPECT_TRUE(field->hasActiveFocus());
  EXPECT_EQ(field->property("displayText").toString(), FieldText("Press shortcut"));

  const auto* hint =
      FindItem(harness.window_, QStringLiteral("shortcutCaptureHint:library.selectAll"));
  ASSERT_NE(hint, nullptr);
  EXPECT_TRUE(hint->property("visible").toBool());
}

TEST(ShortcutSettingsQmlTest, ReturnSavesCandidateWithoutBecomingTheBinding) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));

  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+K")});
  // Return committed the candidate; it never became the stored binding.
  EXPECT_FALSE(registry->keySequenceTexts(QStringLiteral("library.selectAll"))
                   .join(',')
                   .contains(QStringLiteral("Return")));
  EXPECT_FALSE(field->property("capturing").toBool());
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("idle"));
  EXPECT_EQ(field->property("displayText").toString(),
            registry->shortcutText(QStringLiteral("library.selectAll")));
}

TEST(ShortcutSettingsQmlTest, EscapeSavesCandidateWithoutClosingTheDialog) {
  SettingsDialogHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* settings = harness.dialog();
  ASSERT_NE(settings, nullptr);
  harness.OpenAt(7);
  ASSERT_TRUE(settings->property("visible").toBool());

  auto* field = StartDialogCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  ProcessEvents();

  QTest::keyClick(harness.window_, Qt::Key_Escape);
  ProcessEvents();
  // The capture field consumed Escape as a commit, not a dialog close.
  EXPECT_EQ(harness.registry()->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+K")});
  EXPECT_FALSE(field->property("capturing").toBool());
  EXPECT_TRUE(settings->property("visible").toBool());

  QMetaObject::invokeMethod(settings, "close");
}

TEST(ShortcutSettingsQmlTest, SecondEscapeClosesTheDialogAfterCaptureEnds) {
  SettingsDialogHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* settings = harness.dialog();
  ASSERT_NE(settings, nullptr);
  harness.OpenAt(7);
  ASSERT_TRUE(settings->property("visible").toBool());

  auto* field = StartDialogCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  QTest::keyClick(harness.window_, Qt::Key_Escape);
  ProcessEvents();
  ASSERT_TRUE(settings->property("visible").toBool());

  // Capture is over; the next Escape belongs to the dialog's close policy.
  QTest::keyClick(harness.window_, Qt::Key_Escape);
  ProcessEvents();
  EXPECT_FALSE(settings->property("visible").toBool());
}

TEST(ShortcutSettingsQmlTest, PlainDeleteClearsCandidateAndSavePersistsUnassignedState) {
  QTemporaryDir shared_settings;
  ASSERT_TRUE(shared_settings.isValid());
  const QString root = shared_settings.path();

  {
    ShortcutPanelHarness harness(root);
    ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
    auto* registry = harness.registry();
    ASSERT_NE(registry, nullptr);

    auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
    ASSERT_NE(field, nullptr);
    QTest::keyClick(harness.window_, Qt::Key_Delete);
    ProcessEvents();
    EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));
    EXPECT_EQ(field->property("displayText").toString(), FieldText("Unassigned"));

    QTest::keyClick(harness.window_, Qt::Key_Return);
    ProcessEvents();
    EXPECT_TRUE(registry->keySequenceTexts(QStringLiteral("library.selectAll")).isEmpty());
    EXPECT_EQ(field->property("displayText").toString(), RegistryText("Unassigned"));
    EXPECT_FALSE(field->property("assigned").toBool());
  }

  // A fresh engine on the same INI root reloads the unassigned state.
  {
    ShortcutPanelHarness reopened(root);
    ASSERT_NE(reopened.window_, nullptr) << reopened.warnings_.join('\n').toStdString();
    auto* registry = reopened.registry();
    ASSERT_NE(registry, nullptr);
    EXPECT_TRUE(registry->keySequenceTexts(QStringLiteral("library.selectAll")).isEmpty());
    auto* field = FieldForCommand(reopened.window_, QStringLiteral("library.selectAll"));
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->property("displayText").toString(), RegistryText("Unassigned"));
    EXPECT_FALSE(field->property("assigned").toBool());
  }
}

TEST(ShortcutSettingsQmlTest, ModifiedDeleteCanBeCaptured) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  auto* field = StartCapture(harness, QStringLiteral("library.saveProject"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_Delete, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));

  QTest::keyClick(harness.window_, Qt::Key_Enter);
  ProcessEvents();
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.saveProject")),
            QStringList{QStringLiteral("Ctrl+Del")});
  EXPECT_FALSE(field->property("usesDefault").toBool());
}

TEST(ShortcutSettingsQmlTest, ModifierReleaseCapturesShiftForModifierCommands) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  auto* field = StartCapture(harness, QStringLiteral("nodes.extendSelection"));
  ASSERT_NE(field, nullptr);

  QTest::keyPress(harness.window_, Qt::Key_Shift);
  ProcessEvents();
  // The held modifier previews without committing a candidate.
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("capturing"));
  EXPECT_EQ(field->property("displayText").toString(), QStringLiteral("Shift"));

  QTest::keyRelease(harness.window_, Qt::Key_Shift);
  ProcessEvents();
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));
  EXPECT_EQ(field->property("displayText").toString(), QStringLiteral("Shift"));

  QTest::keyClick(harness.window_, Qt::Key_Enter);
  ProcessEvents();
  EXPECT_TRUE(registry->modifierMatches(QStringLiteral("nodes.extendSelection"),
                                        static_cast<int>(Qt::ShiftModifier)));
  EXPECT_EQ(registry->shortcutText(QStringLiteral("nodes.extendSelection")),
            QStringLiteral("Shift"));
  // Saving an explicit modifier binding is a custom override, not a default.
  EXPECT_FALSE(field->property("usesDefault").toBool());
}

TEST(ShortcutSettingsQmlTest, KeyAfterModifierCapturesAChordForKeyCommands) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);

  QTest::keyPress(harness.window_, Qt::Key_Control);
  ProcessEvents();
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("capturing"));
  EXPECT_EQ(field->property("displayText").toString(), QStringLiteral("Ctrl"));

  QTest::keyPress(harness.window_, Qt::Key_K, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));
  EXPECT_EQ(field->property("displayText").toString(), QStringLiteral("Ctrl+K"));

  QTest::keyRelease(harness.window_, Qt::Key_K, Qt::ControlModifier);
  QTest::keyRelease(harness.window_, Qt::Key_Control);
  QTest::keyClick(harness.window_, Qt::Key_Enter);
  ProcessEvents();
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+K")});
  EXPECT_EQ(harness.window_->property("probeActivations").toInt(), 0);
}

TEST(ShortcutSettingsQmlTest, ConflictKeepsCaptureActiveAndNamesTheOtherAction) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  // Ctrl+A already belongs to library.selectAll inside workspace.library.
  auto* field = StartCapture(harness, QStringLiteral("library.saveProject"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_A, Qt::ControlModifier);
  ProcessEvents();

  EXPECT_TRUE(field->property("capturing").toBool());
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("conflict"));
  const QString error = field->property("errorText").toString();
  EXPECT_TRUE(error.contains(QLatin1Char('"') + RegistryText("Select all") +
                             QLatin1Char('"')))
      << error.toStdString();
  EXPECT_EQ(field->property("conflictScope").toString(),
            QStringLiteral("workspace.library"));
  auto* scope_label =
      FindItem(harness.window_, QStringLiteral("shortcutRowErrorScope:library.saveProject"));
  ASSERT_NE(scope_label, nullptr);
  EXPECT_TRUE(scope_label->property("visible").toBool());
  EXPECT_TRUE(scope_label->property("text").toString()
                  .contains(QStringLiteral("workspace.library")));

  // The conflict wrote nothing; Enter re-validates and still fails.
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_TRUE(field->property("capturing").toBool());
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.saveProject")),
            QStringList{QStringLiteral("Ctrl+S")});
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+A")});

  QTest::keyClick(harness.window_, Qt::Key_Tab);
  ProcessEvents();
  EXPECT_FALSE(field->property("capturing").toBool());
}

TEST(ShortcutSettingsQmlTest, PersistenceFailureKeepsThePriorDisplayedBinding) {
  QTemporaryDir shared_settings;
  ASSERT_TRUE(shared_settings.isValid());
  // Block the organization directory with a plain file so the INI cannot be
  // created — the same failure injection ShortcutRegistryTest uses.
  const auto org_dir =
      std::filesystem::path(shared_settings.path().toStdWString()) / "AlcedoTests";
  std::ofstream blocker(org_dir);
  blocker.close();
  ASSERT_TRUE(std::filesystem::exists(org_dir));
  ASSERT_FALSE(std::filesystem::is_directory(org_dir));

  ShortcutPanelHarness harness(shared_settings.path());
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  const QString prior_display =
      FieldForCommand(harness.window_, QStringLiteral("library.selectAll"))
          ->property("displayText")
          .toString();
  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();

  // The write failed; the effective binding and the displayed text stay put.
  EXPECT_EQ(field->property("captureState").toString(),
            QStringLiteral("persistenceError"));
  EXPECT_FALSE(field->property("errorText").toString().isEmpty());
  EXPECT_FALSE(field->property("capturing").toBool());
  EXPECT_EQ(field->property("displayText").toString(), prior_display);
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+A")});
}

TEST(ShortcutSettingsQmlTest, RestoreDefaultRestoresReservedBuiltInKeys) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  const QStringList commands{QStringLiteral("mask.confirmEdit"),
                             QStringLiteral("mask.deleteSelection"),
                             QStringLiteral("nodes.cancel")};
  // keySequenceTexts uses QKeySequence::PortableText ("Del", "Esc", ...).
  const QList<QStringList> expected{{QStringLiteral("Return"), QStringLiteral("Enter")},
                                    {QStringLiteral("Del")},
                                    {QStringLiteral("Esc")}};
  for (int i = 0; i < commands.size(); ++i) {
    const QString& command = commands.at(i);
    auto*            field   = FieldForCommand(harness.window_, command);
    ASSERT_NE(field, nullptr) << command.toStdString();
    ScrollItemIntoView(harness.window_, QStringLiteral("settingsScroll"), field);

    // Clear through the row's Clear action, then restore through the text
    // action that only appears on changed rows.
    auto* clear = FindItem(harness.window_, QStringLiteral("shortcutClearButton:") + command);
    ASSERT_NE(clear, nullptr);
    ClickItem(harness.window_, clear);
    EXPECT_TRUE(registry->keySequenceTexts(command).isEmpty());
    EXPECT_FALSE(field->property("assigned").toBool());

    auto* restore =
        FindItem(harness.window_, QStringLiteral("shortcutRestoreButton:") + command);
    ASSERT_NE(restore, nullptr);
    EXPECT_TRUE(restore->property("visible").toBool());
    ClickItem(harness.window_, restore);
    EXPECT_EQ(registry->keySequenceTexts(command), expected.at(i));
    EXPECT_TRUE(field->property("assigned").toBool());
    EXPECT_TRUE(field->property("usesDefault").toBool());
  }
}

TEST(ShortcutSettingsQmlTest, TabCancelsUnsavedCandidateAndMovesFocus) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);

  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  ProcessEvents();
  ASSERT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));

  QTest::keyClick(harness.window_, Qt::Key_Tab);
  ProcessEvents();
  EXPECT_FALSE(field->property("capturing").toBool());
  EXPECT_FALSE(field->hasActiveFocus());
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("idle"));
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+A")});
  // Focus moved forward in the row's tab order instead of staying on the field.
  auto* clear = FindItem(harness.window_,
                         QStringLiteral("shortcutClearButton:library.selectAll"));
  ASSERT_NE(clear, nullptr);
  EXPECT_TRUE(clear->hasActiveFocus());
}

TEST(ShortcutSettingsQmlTest, SuccessfulChangeUpdatesAnOpenRegisteredShortcut) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  auto* registry = harness.registry();
  ASSERT_NE(registry, nullptr);
  auto* probe = FindItem(harness.window_, QStringLiteral("probeShortcut"));
  ASSERT_NE(probe, nullptr);
  ASSERT_EQ(probe->property("sequences").toStringList(),
            QStringList{QStringLiteral("Ctrl+A")});

  auto* field = StartCapture(harness, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();

  // The open surface re-read the registry without a restart.
  EXPECT_EQ(probe->property("sequences").toStringList(),
            QStringList{QStringLiteral("Ctrl+K")});

  // Old binding is dead, new binding activates the same command.
  QTest::keyClick(harness.window_, Qt::Key_A, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(harness.window_->property("probeActivations").toInt(), 0);
  QTest::keyClick(harness.window_, Qt::Key_K, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_EQ(harness.window_->property("probeActivations").toInt(), 1);
}

TEST(ShortcutSettingsQmlTest, CaptureFieldExposesAccessibleNameDescriptionAndFocus) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();

  auto* field = FieldForCommand(harness.window_, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(field->property("activeFocusOnTab").toBool());
  const QString accessible_name = field->property("accessibleName").toString();
  const QString accessible_desc = field->property("accessibleDescription").toString();
  EXPECT_TRUE(accessible_name.contains(RegistryText("Select all")))
      << accessible_name.toStdString();
  EXPECT_TRUE(accessible_desc.contains(field->property("bindingText").toString()));
  EXPECT_FALSE(accessible_desc.isEmpty());

  // Keyboard-only entry: focus + Enter starts capture without the pointer.
  ScrollItemIntoView(harness.window_, QStringLiteral("settingsScroll"), field);
  field->forceActiveFocus();
  ProcessEvents();
  QTest::keyClick(harness.window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_TRUE(field->property("capturing").toBool());
  // The a11y description reflects the recording state.
  EXPECT_TRUE(field->property("accessibleDescription")
                  .toString()
                  .contains(FieldText("Tab cancels")));
}

TEST(ShortcutSettingsQmlTest, KeyboardPageUsesThemeValuesWithoutRawVisualColors) {
  ShortcutPanelHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  const auto& theme = AppTheme::Instance();

  auto* field = FieldForCommand(harness.window_, QStringLiteral("library.selectAll"));
  ASSERT_NE(field, nullptr);
  auto* box = FindItem(field, QStringLiteral("shortcutCaptureBox:library.selectAll"));
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(box->property("color").value<QColor>(), theme.bgBaseColor());
  // Read the mirrored palette so the check stays a token comparison rather
  // than resolving a nested QML property group.
  EXPECT_EQ(field->property("fillColor").value<QColor>(), theme.bgBaseColor());
  EXPECT_EQ(field->property("borderColor").value<QColor>(), theme.cardBorderColor());
  EXPECT_EQ(field->property("dangerColor").value<QColor>(), theme.dangerColor());

  auto* error_label =
      FindItem(harness.window_, QStringLiteral("shortcutRowError:library.selectAll"));
  ASSERT_NE(error_label, nullptr);
  EXPECT_EQ(error_label->property("color").value<QColor>(), theme.dangerColor());

  // Source-level guard: no hex literals or Material imports may enter the two
  // new production files.
  const QRegularExpression hex_color(QStringLiteral("#(?:[0-9a-fA-F]{3,8})\\b"));
  const QRegularExpression material_import(
      QStringLiteral("import\\s+QtQuick\\.Controls\\.Material"));
  for (const char* file_name : {"ShortcutCaptureField.qml", "KeyboardSettingsPanel.qml"}) {
    QFile source(QString::fromStdString(
        (QmlDirectoryPath() / file_name).string()));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly | QIODevice::Text)) << file_name;
    const QString text = QString::fromUtf8(source.readAll());
    EXPECT_FALSE(hex_color.match(text).hasMatch()) << file_name;
    EXPECT_FALSE(material_import.match(text).hasMatch()) << file_name;
  }
}

}  // namespace
}  // namespace alcedo::ui::test
