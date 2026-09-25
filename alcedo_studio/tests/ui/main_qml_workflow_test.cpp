//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file main_qml_workflow_test.cpp
/// @brief Loads the production Main.qml and verifies its real module wiring.

#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>
#include <QUrl>
#include <QuickQanava>
#include <filesystem>
#include <sstream>
#include <vector>

#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace alcedo::ui::test {
namespace {

using MainQmlWorkflowTests = ApplicationModuleHostTestFixture;

auto MainQmlUrl() -> QUrl {
  const auto path =
      std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" / "Main.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

TEST_F(MainQmlWorkflowTests, ProductionWindowLoadsAndRoutesCoreWorkspaceActions) {
  ASSERT_TRUE(QCoreApplication::instance());
  alcedo::ui::AppTheme::RegisterFonts();

  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));

  alcedo::ui::LanguageManager language_manager(QCoreApplication::instance());
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  QuickQanava::initialize(&engine);
  language_manager.AttachEngine(&engine);
  host.AttachQmlEngine(&engine);
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &host);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                           &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("languageManager"), &language_manager);

  std::vector<QQmlError> qml_warnings;
  QObject::connect(&engine, &QQmlEngine::warnings,
                   [&qml_warnings](const QList<QQmlError>& warnings) {
                     qml_warnings.insert(qml_warnings.end(), warnings.begin(), warnings.end());
                   });

  engine.load(MainQmlUrl());
  if (engine.rootObjects().empty()) {
    std::ostringstream errors;
    for (const auto& warning : qml_warnings) {
      errors << warning.toString().toStdString() << '\n';
    }
    FAIL() << errors.str();
  }

  auto* root = engine.rootObjects().front();
  ASSERT_NE(root, nullptr);
  auto* window = qobject_cast<QQuickWindow*>(root);
  ASSERT_NE(window, nullptr);
  EXPECT_TRUE(window->isVisible());
  EXPECT_EQ(root->objectName(), QStringLiteral("mainWindow"));

  auto* task_bar = root->findChild<QObject*>(QStringLiteral("backgroundTaskBar"));
  ASSERT_NE(task_bar, nullptr);
  EXPECT_FALSE(task_bar->property("layoutActive").toBool());
  BackgroundTaskSnapshot task_snapshot;
  task_snapshot.kind_             = BackgroundTaskKind::ImageAnalysis;
  task_snapshot.state_            = BackgroundTaskState::Running;
  task_snapshot.title_            = QStringLiteral("Analyzing");
  task_snapshot.progress_percent_ = 25;
  task_snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  const QString task_id           = host.background_tasks()->RegisterTask(task_snapshot);
  ProcessEvents(50);
  EXPECT_TRUE(task_bar->property("layoutActive").toBool());
  host.background_tasks()->FinishTask(task_id, BackgroundTaskState::Succeeded);
  ProcessEvents(AppTheme::Instance().backgroundTaskAutoCollapseMs() +
                AppTheme::Instance().motionFoldOpenMs() + 100);
  EXPECT_FALSE(task_bar->property("layoutActive").toBool());

  // Exercise the real project, folder, thumbnail, import, export, inspection,
  // search, settings, and editor-entry seams through their concrete modules.
  host.folders()->SelectFolder(0);
  host.library()->LoadMoreThumbnails();
  host.library()->LoadThumbnailsThroughIndex(0);
  host.import_export()->StartImport(QStringList{});
  host.import_export()->StartExport(PathToQString(temp_dir_));
  host.import_export()->ResetExportState();
  (void)host.images()->GetFocusedImageInspection(0, 0);
  (void)host.search()->RequestSearch(QStringLiteral(""), 0, 24);

  host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(50);
  EXPECT_EQ(host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_FALSE(host.editor_session()->has_image());
  auto* workspace_host = root->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  EXPECT_EQ(workspace_host->property("activeWorkspace").toString(), QStringLiteral("editor"));
  EXPECT_TRUE(workspace_host->property("editorVisible").toBool());
  EXPECT_FALSE(workspace_host->property("libraryVisible").toBool());
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("editorBackgroundTasksRailButton")), nullptr);

  host.workspace_router()->OpenLibrary();
  ProcessEvents(50);
  EXPECT_EQ(host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_FALSE(host.editor_session()->has_image());
  EXPECT_TRUE(workspace_host->property("libraryVisible").toBool());
  EXPECT_FALSE(workspace_host->property("editorVisible").toBool());
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(root, "openSettingsDialog", Q_ARG(QVariant, QVariant(0))));
  ProcessEvents(100);
  auto* settings = root->findChild<QObject*>(QStringLiteral("settingsDialog"));
  ASSERT_NE(settings, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "open"));
  EXPECT_TRUE(settings->property("visible").toBool());
  settings->setProperty("currentCategory", 6);
  ProcessEvents(50);
  auto* updates_panel = settings->findChild<QObject*>(QStringLiteral("updatesSettingsPanel"));
  ASSERT_NE(updates_panel, nullptr);
  auto* update_status = settings->findChild<QObject*>(QStringLiteral("updatesStatusLabel"));
  ASSERT_NE(update_status, nullptr);
  EXPECT_TRUE(update_status->property("visible").toBool());
  EXPECT_FALSE(update_status->property("text").toString().trimmed().isEmpty());
  settings->setProperty("currentCategory", 8);
  ProcessEvents(50);
  auto* version_label = settings->findChild<QObject*>(QStringLiteral("aboutVersionLabel"));
  ASSERT_NE(version_label, nullptr);
  EXPECT_TRUE(version_label->property("text").toString().contains(
      QString::number(host.updates()->current_build())));
  EXPECT_EQ(root->findChild<QObject*>(QStringLiteral("updateOfferDialog")), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "close"));

  auto* collections = root->findChild<QObject*>(QStringLiteral("collectionsPanel"));
  auto* search      = root->findChild<QObject*>(QStringLiteral("globalSearchDialog"));
  auto* analysis    = root->findChild<QObject*>(QStringLiteral("advancedContentAnalysisDialog"));
  auto* background_tasks_dialog =
      root->findChild<QObject*>(QStringLiteral("backgroundTasksDialog"));
  ASSERT_NE(collections, nullptr);
  ASSERT_NE(search, nullptr);
  ASSERT_NE(analysis, nullptr);
  ASSERT_NE(background_tasks_dialog, nullptr);
  EXPECT_EQ(collections->property("folderController").value<QObject*>(), host.folders());
  EXPECT_EQ(search->property("searchController").value<QObject*>(), host.search());
  EXPECT_EQ(analysis->property("imageController").value<QObject*>(), host.images());
  EXPECT_EQ(background_tasks_dialog->property("controller").value<QObject*>(),
            host.background_tasks());

  for (int index = 0; index < 7; ++index) {
    BackgroundTaskSnapshot completed_task;
    completed_task.kind_            = BackgroundTaskKind::SemanticGeneration;
    completed_task.state_           = BackgroundTaskState::Running;
    completed_task.title_           = QStringLiteral("Completed task %1").arg(index);
    completed_task.shutdown_policy_ = BackgroundTaskShutdownPolicy::WaitForFinish;
    const QString completed_task_id = host.background_tasks()->RegisterTask(completed_task);
    host.background_tasks()->FinishTask(completed_task_id, BackgroundTaskState::Succeeded);
  }
  BackgroundTaskSnapshot refreshing_task;
  refreshing_task.kind_             = BackgroundTaskKind::ModelDownload;
  refreshing_task.state_            = BackgroundTaskState::Running;
  refreshing_task.title_            = QStringLiteral("Refreshing task");
  refreshing_task.progress_percent_ = 20;
  refreshing_task.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  const QString refreshing_task_id  = host.background_tasks()->RegisterTask(refreshing_task);

  ASSERT_TRUE(QMetaObject::invokeMethod(collections, "backgroundTasksRequested"));
  ProcessEvents(50);
  EXPECT_TRUE(background_tasks_dialog->property("opened").toBool());
  EXPECT_NE(background_tasks_dialog->property("blurSource").value<QObject*>(), nullptr);

  auto* background_tasks_title =
      root->findChild<QObject*>(QStringLiteral("backgroundTasksDialogTitle"));
  auto* background_tasks_list = root->findChild<QObject*>(QStringLiteral("backgroundTasksList"));
  ASSERT_TRUE(qml_warnings.empty())
      << (qml_warnings.empty() ? std::string{} : qml_warnings.back().toString().toStdString());
  ASSERT_NE(background_tasks_title, nullptr);
  ASSERT_NE(background_tasks_list, nullptr);
  EXPECT_EQ(background_tasks_title->property("font").value<QFont>().pixelSize(),
            AppTheme::Instance().fontSizeHeadline());

  ProcessEvents(200);
  ASSERT_GT(background_tasks_list->property("contentHeight").toReal(),
            background_tasks_list->property("height").toReal());
  ASSERT_TRUE(background_tasks_list->setProperty("contentY", 100.0));
  ProcessEvents(50);
  const qreal task_content_y = background_tasks_list->property("contentY").toReal();
  ASSERT_GT(task_content_y, 0.0);
  host.background_tasks()->UpdateTask(refreshing_task_id, QStringLiteral("Refreshing task"),
                                      QStringLiteral("Halfway"), 50);
  ProcessEvents(50);
  EXPECT_NEAR(background_tasks_list->property("contentY").toReal(), task_content_y, 1.0);
  host.background_tasks()->FinishTask(refreshing_task_id, BackgroundTaskState::Succeeded);

  ASSERT_TRUE(QMetaObject::invokeMethod(background_tasks_dialog, "close"));
  EXPECT_TRUE(qml_warnings.empty()) << qml_warnings.front().toString().toStdString();
}

// Shared boot path for the B1 workflow cases: isolated QSettings (shortcut
// commits must not touch real settings), real module host, and production
// Main.qml. Mirrors the wiring in the load test above.
auto LoadMainQml(ApplicationModuleHost& host, LanguageManager& language_manager,
                 QQmlApplicationEngine& engine, std::vector<QQmlError>& warnings) -> QObject* {
  engine.addImportPath(QStringLiteral("qrc:/"));
  QuickQanava::initialize(&engine);
  language_manager.AttachEngine(&engine);
  host.AttachQmlEngine(&engine);
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &host);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                           &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("languageManager"), &language_manager);
  QObject::connect(&engine, &QQmlEngine::warnings,
                   [&warnings](const QList<QQmlError>& emitted) {
                     warnings.insert(warnings.end(), emitted.begin(), emitted.end());
                   });
  engine.load(MainQmlUrl());
  return engine.rootObjects().empty() ? nullptr : engine.rootObjects().front();
}

TEST_F(MainQmlWorkflowTests, SettingsDoneDoesNotOverwriteCommittedShortcutChanges) {
  ASSERT_TRUE(QCoreApplication::instance());
  // The capture field writes through the registry into QSettings; isolate the
  // store before any module touches it.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     PathToQString(temp_dir_ / "settings"));
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  QCoreApplication::setApplicationName(QStringLiteral("MainQmlWorkflowTest"));
  alcedo::ui::AppTheme::RegisterFonts();

  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  alcedo::ui::LanguageManager language_manager(QCoreApplication::instance());
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QQmlApplicationEngine  engine;
  std::vector<QQmlError> qml_warnings;
  QObject*               root = LoadMainQml(host, language_manager, engine, qml_warnings);
  ASSERT_NE(root, nullptr) << (qml_warnings.empty()
                                   ? std::string{}
                                   : qml_warnings.front().toString().toStdString());
  auto* window = qobject_cast<QQuickWindow*>(root);
  ASSERT_NE(window, nullptr);

  auto* registry =
      engine.singletonInstance<ShortcutRegistry*>(QStringLiteral("Alcedo.Main"),
                                                  QStringLiteral("ShortcutRegistry"));
  ASSERT_NE(registry, nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(root, "openSettingsDialog", Q_ARG(QVariant, QVariant(7))));
  ProcessEvents(100);
  auto* settings = root->findChild<QObject*>(QStringLiteral("settingsDialog"));
  ASSERT_NE(settings, nullptr);
  ASSERT_TRUE(settings->property("visible").toBool());
  EXPECT_EQ(settings->property("currentCategory").toInt(), 7);

  // Commit a real capture through the production field.
  auto* field =
      settings->findChild<QQuickItem*>(QStringLiteral("shortcutCaptureField:library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    field->mapToScene(QPointF(field->width() * 0.5, field->height() * 0.5))
                        .toPoint());
  ProcessEvents(50);
  ASSERT_TRUE(field->property("capturing").toBool());
  QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
  QTest::keyClick(window, Qt::Key_Return);
  ProcessEvents(50);
  ASSERT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+K")});

  // Done applies the dialog's own pending values and closes; the committed
  // binding is untouched — the dialog never staged a second shortcut set.
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "applySettings"));
  ProcessEvents(100);
  EXPECT_FALSE(settings->property("visible").toBool());
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+K")});
  EXPECT_EQ(field->property("displayText").toString(),
            registry->shortcutText(QStringLiteral("library.selectAll")));

  const auto restored = registry->restoreDefault(QStringLiteral("library.selectAll"));
  EXPECT_TRUE(restored.value(QStringLiteral("succeeded")).toBool());
}

TEST_F(MainQmlWorkflowTests, TextInputAndCapturePriorityPreventProductCommandActivation) {
  ASSERT_TRUE(QCoreApplication::instance());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     PathToQString(temp_dir_ / "settings"));
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  QCoreApplication::setApplicationName(QStringLiteral("MainQmlWorkflowTest"));
  alcedo::ui::AppTheme::RegisterFonts();

  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  alcedo::ui::LanguageManager language_manager(QCoreApplication::instance());
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QQmlApplicationEngine  engine;
  std::vector<QQmlError> qml_warnings;
  QObject*               root = LoadMainQml(host, language_manager, engine, qml_warnings);
  ASSERT_NE(root, nullptr) << (qml_warnings.empty()
                                   ? std::string{}
                                   : qml_warnings.front().toString().toStdString());
  auto* window = qobject_cast<QQuickWindow*>(root);
  ASSERT_NE(window, nullptr);

  auto* select_all =
      root->findChild<QQuickItem*>(QStringLiteral("librarySelectAllShortcut"));
  ASSERT_NE(select_all, nullptr);
  QSignalSpy activated_spy(select_all, SIGNAL(activated()));
  ASSERT_TRUE(activated_spy.isValid());

  // Editable text input keeps its native Ctrl+A while the product shortcut
  // stays suppressed — no select-all fires on the library surface.
  auto* search_dialog = root->findChild<QObject*>(QStringLiteral("globalSearchDialog"));
  ASSERT_NE(search_dialog, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(search_dialog, "open"));
  ProcessEvents(100);
  auto* search_field = root->findChild<QQuickItem*>(QStringLiteral("globalSearchField"));
  ASSERT_NE(search_field, nullptr);
  search_field->setProperty("text", QStringLiteral("album cover"));
  search_field->forceActiveFocus();
  ProcessEvents(50);
  EXPECT_FALSE(select_all->property("shortcutEnabled").toBool());
  QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
  ProcessEvents(50);
  EXPECT_EQ(activated_spy.count(), 0);
  EXPECT_EQ(search_field->property("selectedText").toString(),
            QStringLiteral("album cover"));
  ASSERT_TRUE(QMetaObject::invokeMethod(search_dialog, "close"));
  ProcessEvents(100);

  // An active capture row owns the same chord: Ctrl+A becomes the pending
  // candidate instead of activating the library command.
  ASSERT_TRUE(QMetaObject::invokeMethod(root, "openSettingsDialog", Q_ARG(QVariant, QVariant(7))));
  ProcessEvents(100);
  auto* settings = root->findChild<QObject*>(QStringLiteral("settingsDialog"));
  ASSERT_NE(settings, nullptr);
  auto* field =
      settings->findChild<QQuickItem*>(QStringLiteral("shortcutCaptureField:library.selectAll"));
  ASSERT_NE(field, nullptr);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                    field->mapToScene(QPointF(field->width() * 0.5, field->height() * 0.5))
                        .toPoint());
  ProcessEvents(50);
  ASSERT_TRUE(field->property("capturing").toBool());
  QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
  ProcessEvents(50);
  EXPECT_EQ(activated_spy.count(), 0);
  EXPECT_EQ(field->property("captureState").toString(), QStringLiteral("candidate"));
  EXPECT_EQ(field->property("displayText").toString(),
            QStringLiteral("Ctrl+A"));

  // Tab cancels the candidate; the live binding is unchanged.
  QTest::keyClick(window, Qt::Key_Tab);
  ProcessEvents(50);
  auto* registry =
      engine.singletonInstance<ShortcutRegistry*>(QStringLiteral("Alcedo.Main"),
                                                  QStringLiteral("ShortcutRegistry"));
  ASSERT_NE(registry, nullptr);
  EXPECT_EQ(registry->keySequenceTexts(QStringLiteral("library.selectAll")),
            QStringList{QStringLiteral("Ctrl+A")});
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "close"));
}

}  // namespace
}  // namespace alcedo::ui::test
