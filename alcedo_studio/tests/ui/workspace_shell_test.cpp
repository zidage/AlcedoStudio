//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file workspace_shell_test.cpp
/// @brief Verifies Phase 1B workspace routing, lazy load teardown, and filmstrip shell prefs.

#include "ui/album_backend_test_fixture.hpp"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QUrl>

#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"

namespace alcedo::ui::test {
namespace {

auto MainQmlUrl() -> QUrl {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                    "Main.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

class LoadedMainWindow {
 public:
  ApplicationModuleHost host;
  alcedo::ui::LanguageManager language_manager{QCoreApplication::instance()};
  QQmlApplicationEngine engine;
  std::vector<QQmlError> qml_warnings;
  QQuickWindow* window = nullptr;

  LoadedMainWindow() = default;
  LoadedMainWindow(const LoadedMainWindow&) = delete;
  LoadedMainWindow& operator=(const LoadedMainWindow&) = delete;
};

class WorkspaceShellTests : public ApplicationModuleHostTestFixture {
 protected:
  auto LoadMainWindow() -> std::unique_ptr<LoadedMainWindow> {
    auto loaded = std::make_unique<LoadedMainWindow>();
    alcedo::ui::AppTheme::RegisterFonts();
    EXPECT_TRUE(CreateTestProject(loaded->host));

    alcedo::ui::AppTheme::SetEffectiveLanguageCode(
        loaded->language_manager.EffectiveLanguageCode());
    QQuickStyle::setStyle(QStringLiteral("Material"));

    loaded->engine.addImportPath(QStringLiteral("qrc:/"));
    loaded->language_manager.AttachEngine(&loaded->engine);
    loaded->engine.rootContext()->setContextProperty(QStringLiteral("appModules"),
                                                      &loaded->host);
    loaded->engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                                      &alcedo::ui::AppTheme::Instance());
    loaded->engine.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                                      &loaded->language_manager);

    QObject::connect(&loaded->engine, &QQmlEngine::warnings,
                     [raw = loaded.get()](const QList<QQmlError>& warnings) {
                       raw->qml_warnings.insert(raw->qml_warnings.end(), warnings.begin(),
                                                warnings.end());
                     });

    loaded->engine.load(MainQmlUrl());
    if (loaded->engine.rootObjects().empty()) {
      std::ostringstream errors;
      for (const auto& warning : loaded->qml_warnings) {
        errors << warning.toString().toStdString() << '\n';
      }
      ADD_FAILURE() << errors.str();
      return loaded;
    }
    loaded->window = qobject_cast<QQuickWindow*>(loaded->engine.rootObjects().front());
    return loaded;
  }
};

TEST_F(WorkspaceShellTests, WorkspaceRouterOpensEmptyEditorAndReturnsToLibrary) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* router = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  ASSERT_NE(router, nullptr);
  ASSERT_NE(session, nullptr);

  EXPECT_EQ(router->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(session->active());

  router->OpenEditor(0, 0);
  ProcessEvents(50);
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(session->active());
  EXPECT_FALSE(session->has_image());
  EXPECT_EQ(session->element_id(), 0u);
  EXPECT_EQ(session->image_id(), 0u);

  auto* workspace_host = loaded->window->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  EXPECT_EQ(workspace_host->property("activeWorkspace").toString(), QStringLiteral("editor"));
  EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 1);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorEmptyState")), nullptr);

  router->OpenLibrary();
  ProcessEvents(50);
  EXPECT_EQ(router->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(session->active());
  EXPECT_EQ(workspace_host->property("activeWorkspace").toString(), QStringLiteral("library"));
  EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 1);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, WorkspaceRouterOpensEditorFocusedOnElement) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(42, 7);
  ProcessEvents(50);

  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_EQ(loaded->host.workspace_router()->element_id(), 42u);
  EXPECT_EQ(loaded->host.workspace_router()->image_id(), 7u);
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_TRUE(loaded->host.editor_session()->has_image());
  EXPECT_EQ(loaded->host.editor_session()->element_id(), 42u);
  EXPECT_EQ(loaded->host.editor_session()->image_id(), 7u);

  auto* empty = loaded->window->findChild<QObject*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_FALSE(empty->property("visible").toBool());
  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, RepeatedWorkspaceSwitchesKeepSingleActiveLoader) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  auto* workspace_host = loaded->window->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);

  for (int i = 0; i < 8; ++i) {
    loaded->host.workspace_router()->OpenEditor(static_cast<uint>(i + 1),
                                                 static_cast<uint>(i + 10));
    ProcessEvents(20);
    EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 1) << "iter " << i;
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
    EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

    loaded->host.workspace_router()->OpenLibrary();
    ProcessEvents(20);
    EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 1) << "iter " << i;
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
    EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  }
  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, FilmstripCollapseReleasesHeightAndPersistsPreference) {
  ASSERT_TRUE(QCoreApplication::instance());
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  QCoreApplication::setApplicationName(QStringLiteral("AlcedoWorkspaceShellTest"));

  {
    QSettings settings;
    settings.setValue(QStringLiteral("editor/filmstripCollapsed"), false);
    settings.setValue(QStringLiteral("editor/filmstripExpandedHeight"), 140.0);
    settings.sync();
  }

  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(50);

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  // Ensure deterministic expanded height even if a prior process left prefs.
  session->set_filmstrip_collapsed(false);
  session->set_filmstrip_expanded_height(140.0);
  EXPECT_FALSE(session->filmstrip_collapsed());
  EXPECT_DOUBLE_EQ(session->filmstrip_expanded_height(), 140.0);

  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  auto* handle = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(handle, nullptr);
  EXPECT_TRUE(handle->activeFocusOnTab());

  const qreal expanded_height = filmstrip->height();
  EXPECT_GT(expanded_height, handle->height());

  session->set_filmstrip_collapsed(true);
  ProcessEvents(100);
  EXPECT_TRUE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->height(), handle->height(), 1.0);

  // Preference survives editor visual-tree teardown and reopen in the same process.
  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(20);
  loaded->host.workspace_router()->OpenEditor(2, 2);
  ProcessEvents(50);
  EXPECT_TRUE(loaded->host.editor_session()->filmstrip_collapsed());
  EXPECT_DOUBLE_EQ(loaded->host.editor_session()->filmstrip_expanded_height(), 140.0);

  // Preference is durable across a new controller construction (process restart).
  loaded.reset();
  {
    alcedo::ui::EditorSessionController restored(nullptr);
    EXPECT_TRUE(restored.filmstrip_collapsed());
    EXPECT_DOUBLE_EQ(restored.filmstrip_expanded_height(), 140.0);
  }
}

TEST_F(WorkspaceShellTests, EditorSessionControllerTracksSessionWithoutLegacyModal) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));

  host.workspace_router()->OpenEditor(9, 3);
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_TRUE(host.editor_session()->has_image());
  // Phase 1B must not open the legacy modal dialog path.
  EXPECT_FALSE(host.editor()->editor_active());

  host.workspace_router()->OpenLibrary();
  EXPECT_FALSE(host.editor_session()->active());
  EXPECT_FALSE(host.editor_session()->has_image());
  EXPECT_FALSE(host.editor()->editor_active());
}

}  // namespace
}  // namespace alcedo::ui::test
