//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file main_qml_workflow_test.cpp
/// @brief Loads the production Main.qml and verifies its real module wiring.

#include "ui/album_backend_test_fixture.hpp"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>

#include <filesystem>
#include <sstream>
#include <vector>

#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"
#include "ui/alcedo_main/language_manager.hpp"

namespace alcedo::ui::test {
namespace {

using MainQmlWorkflowTests = ApplicationModuleHostTestFixture;

auto MainQmlUrl() -> QUrl {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                    "Main.qml";
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
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(
      language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  language_manager.AttachEngine(&engine);
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &host);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                            &alcedo::ui::AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                            &language_manager);

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

  // Exercise the real project, folder, thumbnail, import, export, inspection,
  // search, settings, and editor-entry seams through their concrete modules.
  host.folders()->SelectFolder(0);
  host.library()->LoadMoreThumbnails();
  host.library()->LoadThumbnailsThroughIndex(0);
  host.import_export()->StartImport(QStringList{});
  host.import_export()->StartExport(PathToQString(temp_dir_));
  host.import_export()->ResetExportState();
  (void)host.images()->GetFocusedImageInspection(0, 0);
  (void)host.search()->SearchPreview(QStringLiteral(""), 0, 24);

  ResetOpenEditorDialogCallCount();
  host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(50);
  EXPECT_EQ(host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_FALSE(host.editor_session()->has_image());
  // Unified workspace route must not open the legacy modal editor.
  EXPECT_FALSE(host.editor()->editor_active());
  EXPECT_EQ(OpenEditorDialogCallCount(), 0);
  auto* workspace_host = root->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  EXPECT_EQ(workspace_host->property("activeWorkspace").toString(), QStringLiteral("editor"));
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_EQ(root->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  host.workspace_router()->OpenLibrary();
  ProcessEvents(50);
  EXPECT_EQ(host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(host.editor_session()->active());
  EXPECT_NE(root->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_EQ(root->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(root, "openSettingsDialog",
                                        Q_ARG(QVariant, QVariant(0))));
  ProcessEvents(100);
  auto* settings = root->findChild<QObject*>(QStringLiteral("settingsDialog"));
  ASSERT_NE(settings, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "open"));
  EXPECT_TRUE(settings->property("visible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "close"));

  auto* collections = root->findChild<QObject*>(QStringLiteral("collectionsPanel"));
  auto* search = root->findChild<QObject*>(QStringLiteral("globalSearchDialog"));
  auto* analysis = root->findChild<QObject*>(QStringLiteral("advancedContentAnalysisDialog"));
  ASSERT_NE(collections, nullptr);
  ASSERT_NE(search, nullptr);
  ASSERT_NE(analysis, nullptr);
  EXPECT_EQ(collections->property("folderController").value<QObject*>(), host.folders());
  EXPECT_EQ(search->property("searchController").value<QObject*>(), host.search());
  EXPECT_EQ(analysis->property("imageController").value<QObject*>(), host.images());
  EXPECT_TRUE(qml_warnings.empty()) << qml_warnings.front().toString().toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
