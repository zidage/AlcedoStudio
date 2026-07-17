//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file workspace_shell_test.cpp
/// @brief Verifies Phase 1B workspace routing, lazy load teardown, filmstrip shell
/// prefs, project-switch session seal, library view-state restore, and real QML
/// interaction entrypoints.

#include "ui/album_backend_test_fixture.hpp"

#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"
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

class ScopedIniSettings {
 public:
  ScopedIniSettings(const std::filesystem::path& dir, const QString& org, const QString& app)
      : prev_org_(QCoreApplication::organizationName()),
        prev_app_(QCoreApplication::applicationName()),
        prev_format_(QSettings::defaultFormat()) {
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    settings_root_ = QString::fromStdWString(dir.wstring());
#else
    settings_root_ = QString::fromStdString(dir.string());
#endif
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_root_);
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setApplicationName(app);
  }

  ~ScopedIniSettings() {
    {
      QSettings settings;
      settings.clear();
      settings.sync();
    }
    QCoreApplication::setOrganizationName(prev_org_);
    QCoreApplication::setApplicationName(prev_app_);
    QSettings::setDefaultFormat(prev_format_);
  }

  ScopedIniSettings(const ScopedIniSettings&) = delete;
  ScopedIniSettings& operator=(const ScopedIniSettings&) = delete;

 private:
  QString prev_org_;
  QString prev_app_;
  QSettings::Format prev_format_;
  QString settings_root_;
};

auto CenterOfItem(QQuickItem* item) -> QPoint {
  const QPointF scene = item->mapToScene(QPointF(item->width() * 0.5, item->height() * 0.5));
  return scene.toPoint();
}

void SeedLibraryThumbnails(ApplicationModuleHost& host, int count, int content_height_hint = 0) {
  std::vector<AlbumItem> items;
  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    AlbumItem item;
    item.element_id = static_cast<sl_element_id_t>(1000 + i);
    item.image_id = static_cast<image_id_t>(2000 + i);
    item.file_id = static_cast<sl_element_id_t>(3000 + i);
    item.file_name = QStringLiteral("test_%1.arw").arg(i);
    item.extension = QStringLiteral("arw");
    item.thumb_data_url = QStringLiteral("data:image/png;base64,");
    items.push_back(item);
  }
  host.library()->view_state().all_images_ = items;
  host.library()->view_state().total_count_ = items.size();
  host.library()->model().resetModel(items, items.size());
  host.library()->NotifyThumbnailsChanged();
  host.library()->NotifyCountsChanged();
  Q_UNUSED(content_height_hint);
}

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
    if (loaded->window) {
      loaded->window->show();
      loaded->window->requestActivate();
    }
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

TEST_F(WorkspaceShellTests, RepeatedWorkspaceSwitchesReturnToObjectBaseline) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  auto* workspace_host = loaded->window->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);

  ProcessEvents(30);
  const int baseline_timers =
      loaded->window->findChildren<QTimer*>(Qt::FindChildrenRecursively).size();
  const int library_creates_before = workspace_host->property("libraryCreateCount").toInt();
  const int library_destroys_before = workspace_host->property("libraryDestroyCount").toInt();

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

  ProcessEvents(30);
  const int library_creates = workspace_host->property("libraryCreateCount").toInt();
  const int library_destroys = workspace_host->property("libraryDestroyCount").toInt();
  const int editor_creates = workspace_host->property("editorCreateCount").toInt();
  const int editor_destroys = workspace_host->property("editorDestroyCount").toInt();

  // One live library instance remains; every prior library and every editor must be destroyed.
  // Initial create is already in library_creates_before; 8 switches add 8 more creates and 8 destroys.
  EXPECT_EQ(library_creates - library_creates_before, 8);
  EXPECT_EQ(library_destroys - library_destroys_before, 8);
  EXPECT_EQ(library_creates, library_destroys + 1);
  EXPECT_EQ(editor_creates, 8);
  EXPECT_EQ(editor_destroys, 8);
  EXPECT_EQ(editor_creates, editor_destroys);

  const int after_timers =
      loaded->window->findChildren<QTimer*>(Qt::FindChildrenRecursively).size();
  EXPECT_EQ(after_timers, baseline_timers);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, FilmstripCollapseViaKeyboardPersistsInIsolatedSettings) {
  ASSERT_TRUE(QCoreApplication::instance());
  ScopedIniSettings settings_scope(temp_dir_ / "qml_settings", QStringLiteral("AlcedoTestOrg"),
                                   QStringLiteral("AlcedoWorkspaceShellTest"));

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

  // Real keyboard entry on the handle (Space), not a direct C++ property poke.
  handle->forceActiveFocus();
  ProcessEvents(20);
  ASSERT_TRUE(handle->hasActiveFocus());
  QTest::keyClick(loaded->window, Qt::Key_Space);
  ProcessEvents(100);
  EXPECT_TRUE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->height(), handle->height(), 1.0);

  // Expand again with Enter.
  QTest::keyClick(loaded->window, Qt::Key_Return);
  ProcessEvents(100);
  EXPECT_FALSE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->height(), expanded_height, 1.0);

  // Collapse with Down while expanded.
  QTest::keyClick(loaded->window, Qt::Key_Down);
  ProcessEvents(100);
  EXPECT_TRUE(session->filmstrip_collapsed());

  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(20);
  loaded->host.workspace_router()->OpenEditor(2, 2);
  ProcessEvents(50);
  EXPECT_TRUE(loaded->host.editor_session()->filmstrip_collapsed());
  EXPECT_DOUBLE_EQ(loaded->host.editor_session()->filmstrip_expanded_height(), 140.0);

  loaded.reset();
  {
    alcedo::ui::EditorSessionController restored(nullptr);
    EXPECT_TRUE(restored.filmstrip_collapsed());
    EXPECT_DOUBLE_EQ(restored.filmstrip_expanded_height(), 140.0);
  }
}

TEST_F(WorkspaceShellTests, EditorSessionControllerTracksSessionWithoutLegacyModal) {
  ResetOpenEditorDialogCallCount();
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));

  host.workspace_router()->OpenEditor(9, 3);
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_TRUE(host.editor_session()->has_image());
  EXPECT_FALSE(host.editor()->editor_active());
  EXPECT_EQ(OpenEditorDialogCallCount(), 0);

  host.workspace_router()->OpenLibrary();
  EXPECT_FALSE(host.editor_session()->active());
  EXPECT_FALSE(host.editor_session()->has_image());
  EXPECT_FALSE(host.editor()->editor_active());
  EXPECT_EQ(OpenEditorDialogCallCount(), 0);
}

TEST_F(WorkspaceShellTests, ProjectSwitchFromEditorEndsSessionAndReturnsToLibrary) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(42, 7);
  ProcessEvents(50);
  ASSERT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  ASSERT_TRUE(loaded->host.editor_session()->active());
  ASSERT_EQ(loaded->host.editor_session()->element_id(), 42u);
  ASSERT_EQ(loaded->host.editor_session()->image_id(), 7u);

  // Create a second project (project switch). finalize_editor_session must seal
  // the QML session and route back to the library before UI state is cleared.
  ASSERT_TRUE(CreateTestProject(loaded->host, QStringLiteral("ui_test_project_b")));
  ProcessEvents(50);

  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_EQ(loaded->host.workspace_router()->element_id(), 0u);
  EXPECT_EQ(loaded->host.workspace_router()->image_id(), 0u);
  EXPECT_FALSE(loaded->host.editor_session()->active());
  EXPECT_EQ(loaded->host.editor_session()->element_id(), 0u);
  EXPECT_EQ(loaded->host.editor_session()->image_id(), 0u);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
}

TEST_F(WorkspaceShellTests, LibraryViewStateSurvivesEditorRoundTrip) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  auto* library = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(library, nullptr);

  // Non-default settings via the live library surface; write-through updates the
  // Main shell so values survive Loader destroy/recreate.
  ASSERT_TRUE(library->setProperty("gridMode", false));
  ASSERT_TRUE(library->setProperty("gridZoomLevel", 1));
  ASSERT_TRUE(library->setProperty("inspectorVisible", false));
  ASSERT_TRUE(library->setProperty("inspectorWidth", 360.0));
  loaded->window->setProperty("libraryGridContentY", 240.0);
  loaded->window->setProperty("libraryListContentY", 180.0);
  ProcessEvents(20);

  EXPECT_FALSE(library->property("gridMode").toBool());
  EXPECT_EQ(library->property("gridZoomLevel").toInt(), 1);
  EXPECT_FALSE(library->property("inspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(library->property("inspectorWidth").toDouble(), 360.0);
  EXPECT_FALSE(loaded->window->property("libraryGridMode").toBool());
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(loaded->window->property("libraryInspectorWidth").toDouble(), 360.0);

  loaded->host.workspace_router()->OpenEditor(11, 22);
  ProcessEvents(40);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  // Shell properties must still hold the values after library destruction.
  EXPECT_FALSE(loaded->window->property("libraryGridMode").toBool());
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(loaded->window->property("libraryInspectorWidth").toDouble(), 360.0);
  EXPECT_DOUBLE_EQ(loaded->window->property("libraryListContentY").toDouble(), 180.0);

  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(50);

  auto* restored = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(restored, nullptr);
  EXPECT_FALSE(restored->property("gridMode").toBool());
  EXPECT_EQ(restored->property("gridZoomLevel").toInt(), 1);
  EXPECT_FALSE(restored->property("inspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(restored->property("inspectorWidth").toDouble(), 360.0);
}

TEST_F(WorkspaceShellTests, InspectorAdaptiveWidthUsesWorkspaceWidthNearMinimum) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  auto* library = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(library, nullptr);

  // Workspace content width is window width minus Main's 12px*2 margins.
  // At the original formula, adaptive max used window width - 24 - panes.
  // After extraction, workspace width already excludes those 24px, so the formula
  // must not subtract them again.
  const qreal left_pane = 276.0;
  const qreal center_min = 560.0;
  const qreal spacing = 36.0;
  const qreal handle = 5.0;

  struct Case {
    int window_width;
  };
  const Case cases[] = {{960}, {1000}, {1100}};
  for (const auto& c : cases) {
    loaded->window->resize(c.window_width, 700);
    ProcessEvents(40);
    const qreal workspace_width = library->width();
    EXPECT_NEAR(workspace_width, static_cast<qreal>(c.window_width) - 24.0, 1.0)
        << "window " << c.window_width;

    const qreal expected = std::max(
        0.0, workspace_width - left_pane - center_min - spacing - handle);
    const qreal actual = library->property("inspectorAdaptiveMaxWidth").toReal();
    EXPECT_NEAR(actual, expected, 1.0) << "window " << c.window_width;

    // Wrong formula that still subtracts window margins would be 24px smaller.
    const qreal wrong = std::max(
        0.0, workspace_width - left_pane - center_min - 24.0 - spacing - handle);
    if (expected > 0.0) {
      EXPECT_GT(actual + 0.5, wrong) << "window " << c.window_width;
    }
  }
}

TEST_F(WorkspaceShellTests, InspectorToggleButtonLivesOnTopToolbarWithOriginalSize) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(30);

  auto* toggle = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryInspectorToggle"));
  ASSERT_NE(toggle, nullptr);
  EXPECT_TRUE(toggle->isVisible());
  EXPECT_NEAR(toggle->width(), 52.0, 1.0);
  EXPECT_NEAR(toggle->height(), 42.0, 1.0);

  // Button must not live under the library browser tree as a smaller control.
  auto* library = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(library, nullptr);
  EXPECT_EQ(library->findChild<QObject*>(QStringLiteral("libraryInspectorToggle")), nullptr);

  // Geometry: toggle should be in the top toolbar band (y near top of content).
  const QPointF scene = toggle->mapToScene(QPointF(0, 0));
  EXPECT_LT(scene.y(), 80.0);

  EXPECT_TRUE(loaded->window->property("libraryInspectorVisible").toBool());
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(toggle));
  ProcessEvents(30);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());
  EXPECT_FALSE(library->property("inspectorVisible").toBool());
}

TEST_F(WorkspaceShellTests, DeferredThumbnailReleasesFlushWhenLibraryDestroyedDuringZoom) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  SeedLibraryThumbnails(loaded->host, 4);
  ProcessEvents(50);

  auto* grid = loaded->window->findChild<QObject*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(grid, nullptr);

  constexpr uint kElementId = 1000;
  constexpr uint kImageId = 2000;
  constexpr uint kMaxEdge = 512;
  loaded->host.library()->SetThumbnailVisible(kElementId, kImageId, true, kMaxEdge);
  ASSERT_TRUE(loaded->host.library()->thumbs().IsThumbnailPinned(kElementId));

  // Simulate mid-zoom deferred release: suspend bindings, queue a release, then
  // destroy the library by opening the editor before the resume timer fires.
  ASSERT_TRUE(QMetaObject::invokeMethod(grid, "beginThumbnailBindingSuspension"));
  ASSERT_TRUE(QMetaObject::invokeMethod(
      grid, "deferThumbnailRelease", Q_ARG(QVariant, QVariant::fromValue(kElementId)),
      Q_ARG(QVariant, QVariant::fromValue(kImageId)),
      Q_ARG(QVariant, QVariant::fromValue(kMaxEdge))));
  ProcessEvents(10);

  loaded->host.workspace_router()->OpenEditor(kElementId, kImageId);
  ProcessEvents(50);

  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryThumbnailGridView")),
            nullptr);
  EXPECT_FALSE(loaded->host.library()->thumbs().IsThumbnailPinned(kElementId));
}

TEST_F(WorkspaceShellTests, RealQmlEntrypointsDriveRoutingFocusAndFilmstripHeight) {
  ASSERT_TRUE(QCoreApplication::instance());
  ResetOpenEditorDialogCallCount();
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  SeedLibraryThumbnails(loaded->host, 6);
  ProcessEvents(80);

  auto* grid_item =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(grid_item, nullptr);

  // Double-click the grid surface. With a populated model the first cell is at
  // the top-left of the grid; this exercises the real MouseArea double-click path.
  const QPoint grid_point = grid_item->mapToScene(QPointF(48, 48)).toPoint();
  QTest::mouseDClick(loaded->window, Qt::LeftButton, Qt::NoModifier, grid_point);
  ProcessEvents(80);

  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_EQ(OpenEditorDialogCallCount(), 0);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);

  auto* back =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorBackToLibraryButton"));
  ASSERT_NE(back, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(back));
  ProcessEvents(80);

  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(loaded->host.editor_session()->active());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  // Empty editor + filmstrip handle keyboard path.
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(50);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());

  auto* handle = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  ASSERT_NE(handle, nullptr);
  ASSERT_NE(filmstrip, nullptr);
  loaded->host.editor_session()->set_filmstrip_collapsed(false);
  ProcessEvents(20);
  const qreal expanded = filmstrip->height();
  handle->forceActiveFocus();
  ProcessEvents(10);
  QTest::keyClick(loaded->window, Qt::Key_Space);
  ProcessEvents(80);
  EXPECT_TRUE(loaded->host.editor_session()->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->height(), handle->height(), 1.0);
  EXPECT_LT(filmstrip->height() + 1.0, expanded);
  EXPECT_EQ(OpenEditorDialogCallCount(), 0);
}

}  // namespace
}  // namespace alcedo::ui::test
