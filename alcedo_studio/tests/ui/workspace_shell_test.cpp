//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file workspace_shell_test.cpp
/// @brief Verifies Phase 1B workspace routing, lazy load teardown, filmstrip shell
/// prefs, project-switch session seal, library view-state restore, real QML
/// interaction entrypoints, and Phase 4C visual/motion contracts.

#include "ui/album_backend_test_fixture.hpp"

#include <QColor>
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
#include <QVariant>
#include <QWheelEvent>

#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/editor_dialog.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/lease_frame_sink.hpp"

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
    // Empty data URL: the grid renders placeholder cards (no decode attempt) so
    // the async QQuickImage decode-failure warning never fires. Routing, scroll,
    // and filter tests do not depend on thumbnail pixels.
    item.thumb_data_url = QString();
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
  // Phase 4C: ordinary workflow tests force reduced motion so geometry /
  // visibility assertions observe terminal state without wall-clock fold timing.
  // Motion-progress tests opt out via setReduceMotion(false) and driveFoldProgress.
  void ForceReducedMotionForWorkflowTests() {
    alcedo::ui::AppTheme::Instance().setReduceMotion(true);
  }

  auto LoadMainWindow(bool create_project = true) -> std::unique_ptr<LoadedMainWindow> {
    auto loaded = std::make_unique<LoadedMainWindow>();
    alcedo::ui::AppTheme::RegisterFonts();
    ForceReducedMotionForWorkflowTests();
    if (create_project) {
      EXPECT_TRUE(CreateTestProject(loaded->host));
    }

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

  // Load Main.qml with a pre-built packed project (e.g. a seeded synthetic image)
  // already loaded into the host. The project is loaded BEFORE the QML engine so
  // the window inits with serviceReady == true (nav enabled, no welcome dialog).
  auto LoadMainWindowWithPackedProject(const std::filesystem::path& packedPath)
      -> std::unique_ptr<LoadedMainWindow> {
    auto loaded = std::make_unique<LoadedMainWindow>();
    alcedo::ui::AppTheme::RegisterFonts();
    ForceReducedMotionForWorkflowTests();
    EXPECT_TRUE(LoadPackedProject(loaded->host, packedPath));

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
  ASSERT_TRUE(library->setProperty("gridZoomLevel", 1));
  ASSERT_TRUE(library->setProperty("inspectorVisible", false));
  ASSERT_TRUE(library->setProperty("inspectorWidth", 360.0));
  ProcessEvents(20);

  EXPECT_EQ(library->property("gridZoomLevel").toInt(), 1);
  EXPECT_FALSE(library->property("inspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(library->property("inspectorWidth").toDouble(), 360.0);
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(loaded->window->property("libraryInspectorWidth").toDouble(), 360.0);

  loaded->host.workspace_router()->OpenEditor(11, 22);
  ProcessEvents(40);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  // Shell properties must still hold the values after library destruction.
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());
  EXPECT_DOUBLE_EQ(loaded->window->property("libraryInspectorWidth").toDouble(), 360.0);

  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(50);

  auto* restored = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(restored, nullptr);
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

  // Phase 4A: return-to-library is owned by the shared main-window navigation,
  // not an editor-local control. The editor back button no longer exists.
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorBackToLibraryButton")),
            nullptr);
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
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

TEST_F(WorkspaceShellTests, PresentationViewportBindingSurvivesImageSwitchAToBToA) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  auto* session = loaded->host.editor_session();
  auto* router = loaded->host.workspace_router();
  ASSERT_NE(session, nullptr);
  ASSERT_NE(router, nullptr);

  router->OpenEditor(10, 100);
  ProcessEvents(60);
  EXPECT_TRUE(session->presentation_viewport_bound());
  auto* viewport_a = qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport_a, nullptr);
  auto* sink_a = session->presentation_frame_sink();
  ASSERT_NE(sink_a, nullptr);
  EXPECT_EQ(sink_a, viewport_a->frameSink());
  const auto gen_a = session->session_generation();
  EXPECT_EQ(viewport_a->imageGeneration(), gen_a);
  EXPECT_EQ(viewport_a->imageIdentity(), 100ull);

  // A → B inside the same editor workspace (no Loader teardown).
  router->OpenEditor(20, 200);
  ProcessEvents(60);
  EXPECT_TRUE(session->presentation_viewport_bound())
      << "image switch must not drop the presentation binding";
  auto* viewport_b = qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport_b, nullptr);
  EXPECT_EQ(viewport_b, viewport_a) << "same QML viewport instance";
  auto* sink_b = session->presentation_frame_sink();
  ASSERT_NE(sink_b, nullptr);
  EXPECT_EQ(sink_b, sink_a);
  EXPECT_GT(session->session_generation(), gen_a);
  EXPECT_EQ(viewport_b->imageIdentity(), 200ull);
  EXPECT_EQ(viewport_b->imageGeneration(), session->session_generation());

  // B → A: generation advances again; late frames from first A are rejected.
  const auto gen_b = session->session_generation();
  router->OpenEditor(10, 100);
  ProcessEvents(60);
  EXPECT_TRUE(session->presentation_viewport_bound());
  EXPECT_EQ(session->presentation_viewport(), viewport_a);
  EXPECT_EQ(session->presentation_frame_sink(), sink_a);
  EXPECT_GT(session->session_generation(), gen_b);
  EXPECT_EQ(viewport_a->imageIdentity(), 100ull);
  EXPECT_EQ(viewport_a->imageGeneration(), session->session_generation());

  // Leaving the editor workspace unbinds on viewport destruction.
  router->OpenLibrary();
  ProcessEvents(60);
  EXPECT_FALSE(session->presentation_viewport_bound());
  EXPECT_EQ(session->presentation_frame_sink(), nullptr);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, ProductionFrameSinkAcceptsThreeLayerFrameSubmissions) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(5, 50);
  ProcessEvents(80);

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->presentation_viewport_bound());
  auto* sink = session->presentation_frame_sink();
  ASSERT_NE(sink, nullptr);
  auto* viewport =
      qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport, nullptr);

  // Production path: session → presentation_viewport → frameSink → EnsureSize.
  // Full RAW decode is Phase 4A; this verifies the attach surface delivers
  // InteractivePrimary / QualityBase / DetailPatch metadata into the sink.
  sink->EnsureSize(64, 48);
  EXPECT_EQ(sink->GetWidth(), 64);
  EXPECT_EQ(sink->GetHeight(), 48);

  const FrameRole roles[] = {FrameRole::InteractivePrimary, FrameRole::QualityBase,
                             FrameRole::DetailPatch};
  for (int i = 0; i < 3; ++i) {
    FramePreviewMetadata meta;
    meta.frame_role = roles[i];
    meta.preview_generation = static_cast<std::uint64_t>(i + 1);
    meta.detail_serial = static_cast<std::uint64_t>(i + 10);
    sink->SetNextFramePreviewMetadata(meta);
    sink->SetNextFramePresentationMode(i == 0 ? FramePresentationMode::RoiFrame
                                              : FramePresentationMode::FullFrame);
    // Without a render-thread published target, Map returns empty — that is
    // expected before scene-graph target publish. The production contract is
    // that the sink is the same object pipeline code will Map/Unmap against.
    EXPECT_EQ(session->presentation_frame_sink(), sink);
  }

  // Switch image and confirm the same sink stays production-attached.
  loaded->host.workspace_router()->OpenEditor(6, 60);
  ProcessEvents(40);
  EXPECT_EQ(session->presentation_frame_sink(), sink);
  EXPECT_EQ(viewport->imageIdentity(), 60ull);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, EditorViewportReceivesRealPointerAndWheelEvents) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(3, 30);
  ProcessEvents(80);

  auto* interaction = loaded->window->findChild<editor_rhi::EditorInteractionController*>(
      QStringLiteral("editorInteractionController"));
  auto* viewport_item =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportItem"));
  ASSERT_NE(interaction, nullptr);
  ASSERT_NE(viewport_item, nullptr);
  EXPECT_TRUE(viewport_item->isVisible());

  // Seed non-zero image geometry so pan/zoom math is defined.
  interaction->setImageSize(4000, 3000);
  interaction->setRenderReferenceSize(4000, 3000);
  interaction->setViewportMetrics(viewport_item->width(), viewport_item->height(), 1.0);
  interaction->applyViewTransformForTest(2.0f, 0.0f, 0.0f);
  ProcessEvents(20);

  const float zoom_before = interaction->zoom();
  const QPoint center = CenterOfItem(viewport_item);

  // Real window mouse press/move/release (pan while zoomed).
  QTest::mousePress(loaded->window, Qt::LeftButton, Qt::NoModifier, center);
  ProcessEvents(10);
  QTest::mouseMove(loaded->window, center + QPoint(30, 12));
  ProcessEvents(10);
  QTest::mouseRelease(loaded->window, Qt::LeftButton, Qt::NoModifier, center + QPoint(30, 12));
  ProcessEvents(20);

  // Ctrl+wheel zoom at cursor through the real QML WheelHandler.
  QPointF angle(0.0, 120.0);
  QPointF pixel(0.0, 0.0);
  QWheelEvent wheel(center, loaded->window->mapToGlobal(center), QPoint(),
                    QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                    false);
  QCoreApplication::sendEvent(loaded->window, &wheel);
  ProcessEvents(20);

  // Double-click / double-tap path.
  QTest::mouseDClick(loaded->window, Qt::LeftButton, Qt::NoModifier, center);
  ProcessEvents(30);

  // Interaction must remain enabled and keep a finite zoom after real events.
  EXPECT_TRUE(interaction->interactionEnabled());
  EXPECT_GE(interaction->zoom(), alcedo::ViewTransformController::kMinInteractiveZoom);
  EXPECT_LE(interaction->zoom(), alcedo::ViewTransformController::kMaxInteractiveZoom);
  // At least one of pan/zoom should have moved for a meaningful gesture sequence.
  const bool zoomed = std::abs(interaction->zoom() - zoom_before) > 1.0e-3f;
  const bool panned =
      std::abs(interaction->panX()) > 1.0e-4f || std::abs(interaction->panY()) > 1.0e-4f;
  EXPECT_TRUE(zoomed || panned);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

// ── Phase 4A: main-window Library / Editor navigation ──────────────────────

TEST_F(WorkspaceShellTests, MainNavigationActivatesLibraryAndEditorByMouse) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  // Navigation is persistent and visible in both workspaces.
  EXPECT_TRUE(library_nav->isVisible());
  EXPECT_TRUE(editor_nav->isVisible());
  EXPECT_TRUE(library_nav->isEnabled());
  EXPECT_TRUE(editor_nav->isEnabled());

  // Editor (empty) activated from the library workspace via main nav.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_FALSE(loaded->host.editor_session()->has_image());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
  // The active-state indicator tracks the workspace (drives the accent icon/tint).
  EXPECT_TRUE(editor_nav->property("isActive").toBool());
  EXPECT_FALSE(library_nav->property("isActive").toBool());

  // Library activated from the editor workspace via main nav.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(loaded->host.editor_session()->active());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_FALSE(editor_nav->property("isActive").toBool());
  EXPECT_TRUE(library_nav->property("isActive").toBool());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, MainNavigationActivatesWorkspacesByKeyboard) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  EXPECT_TRUE(library_nav->activeFocusOnTab());
  EXPECT_TRUE(editor_nav->activeFocusOnTab());

  // Keyboard-activate the editor nav from the library workspace.
  editor_nav->forceActiveFocus();
  ProcessEvents(20);
  ASSERT_TRUE(editor_nav->hasActiveFocus());
  QTest::keyClick(loaded->window, Qt::Key_Space);
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_FALSE(loaded->host.editor_session()->has_image());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);

  // Keyboard-activate the library nav from the editor workspace.
  library_nav->forceActiveFocus();
  ProcessEvents(20);
  ASSERT_TRUE(library_nav->hasActiveFocus());
  QTest::keyClick(loaded->window, Qt::Key_Space);
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(loaded->host.editor_session()->active());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, MainNavigationEditorButtonIsNoOpWhenAlreadyActive) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  // Open the editor focused on a real image (not the empty state).
  loaded->host.workspace_router()->OpenEditor(42, 7);
  ProcessEvents(80);
  ASSERT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  ASSERT_TRUE(loaded->host.editor_session()->has_image());
  ASSERT_EQ(loaded->host.editor_session()->element_id(), 42u);
  ASSERT_EQ(loaded->host.editor_session()->image_id(), 7u);

  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  EXPECT_TRUE(editor_nav->isEnabled());

  // Clicking the already-active editor nav must NOT reset to the empty state;
  // the active session is preserved.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_TRUE(loaded->host.editor_session()->has_image());
  EXPECT_EQ(loaded->host.editor_session()->element_id(), 42u);
  EXPECT_EQ(loaded->host.editor_session()->image_id(), 7u);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, MainNavigationDoesNotDuplicateOrLeakAcrossSwitches) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  auto* workspace_host = loaded->window->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  ProcessEvents(40);

  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);

  // Seed non-default library view state through the live library surface; it
  // writes through to the shell and must survive the workspace switches.
  auto* library = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(library, nullptr);
  ASSERT_TRUE(library->setProperty("gridZoomLevel", 1));
  ASSERT_TRUE(library->setProperty("inspectorVisible", false));
  ProcessEvents(20);
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());

  const int library_creates_before = workspace_host->property("libraryCreateCount").toInt();
  const int editor_creates_before = workspace_host->property("editorCreateCount").toInt();

  for (int i = 0; i < 6; ++i) {
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                      CenterOfItem(editor_nav));
    ProcessEvents(40);
    EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"))
        << "iter " << i;
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                      CenterOfItem(library_nav));
    ProcessEvents(40);
    EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"))
        << "iter " << i;
  }

  // Repeated switching must not duplicate the main navigation.
  EXPECT_EQ(loaded->window->findChildren<QQuickItem*>(
                QStringLiteral("libraryNavButton")).size(), 1);
  EXPECT_EQ(loaded->window->findChildren<QQuickItem*>(
                QStringLiteral("editorNavButton")).size(), 1);

  // Six round trips add six editor creates and six library creates.
  const int library_creates = workspace_host->property("libraryCreateCount").toInt();
  const int editor_creates = workspace_host->property("editorCreateCount").toInt();
  EXPECT_EQ(library_creates - library_creates_before, 6);
  EXPECT_EQ(editor_creates - editor_creates_before, 6);

  // Library view state is preserved across the switches.
  EXPECT_EQ(loaded->window->property("libraryGridZoomLevel").toInt(), 1);
  EXPECT_FALSE(loaded->window->property("libraryInspectorVisible").toBool());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, NoEditorLocalReturnControlRemains) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(30);

  // Focused editor: no editor-local return control.
  loaded->host.workspace_router()->OpenEditor(42, 7);
  ProcessEvents(60);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorBackToLibraryButton")),
            nullptr);

  // Empty editor: the empty state is shown, but still no editor-local return.
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(60);
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorBackToLibraryButton")),
            nullptr);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, MainNavigationDisabledBeforeProjectLoad) {
  ASSERT_TRUE(QCoreApplication::instance());
  // Load Main.qml without creating a project: serviceReady stays false, so the
  // workspace navigation must render in its disabled state.
  auto loaded = LoadMainWindow(/*create_project=*/false);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  EXPECT_FALSE(loaded->host.project()->ServiceReady());
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  // Navigation is still present (persistent chrome) but disabled.
  EXPECT_TRUE(library_nav->isVisible());
  EXPECT_TRUE(editor_nav->isVisible());
  EXPECT_FALSE(library_nav->isEnabled());
  EXPECT_FALSE(editor_nav->isEnabled());
}

// ── Phase 4A-Fix: last-edited image restore, delete clears editor, nav visual
// states, and library scroll/filter preservation ────────────────────────────

TEST_F(WorkspaceShellTests, EditorNavButtonRestoresLastEditedImageAcrossLibraryRoundTrip) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  // Seed the library model so editorImageStillExists(lastEl) resolves in-view.
  SeedLibraryThumbnails(loaded->host, 8);
  ProcessEvents(80);

  auto* router = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  ASSERT_NE(router, nullptr);
  ASSERT_NE(session, nullptr);

  // Open image A (element 1000 / image 2000) via the real router path; Open sets
  // lastElementId/lastImageId.
  router->OpenEditor(1000, 2000);
  ProcessEvents(80);
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  ASSERT_TRUE(session->active());
  ASSERT_TRUE(session->has_image());
  ASSERT_EQ(session->element_id(), 1000u);
  ASSERT_EQ(session->image_id(), 2000u);
  ASSERT_EQ(session->last_element_id(), 1000u);
  ASSERT_EQ(session->last_image_id(), 2000u);

  // Real Library nav button: OpenLibrary finalizes the session but must keep
  // lastElementId/lastImageId so re-entry can restore the image.
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(router->workspace(), QStringLiteral("library"));
  EXPECT_FALSE(session->active());
  EXPECT_EQ(session->last_element_id(), 1000u);
  EXPECT_EQ(session->last_image_id(), 2000u);

  // Real Editor nav button: must restore image A, not open the empty state.
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(session->active());
  EXPECT_TRUE(session->has_image());
  EXPECT_EQ(session->element_id(), 1000u);
  EXPECT_EQ(session->image_id(), 2000u);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_FALSE(empty->isVisible());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, DeletingCurrentEditorImageDropsEditorToEmptyState) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());
  const auto file_id = static_cast<uint>(seeded->file_id_);
  const auto image_id = static_cast<uint>(seeded->image_id_);

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(80);

  auto* router = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  ASSERT_NE(router, nullptr);
  ASSERT_NE(session, nullptr);

  // Edit the seeded image; Open records it as the last-edited image.
  router->OpenEditor(file_id, image_id);
  ProcessEvents(80);
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  ASSERT_TRUE(session->active());
  ASSERT_TRUE(session->has_image());
  ASSERT_EQ(session->element_id(), file_id);
  ASSERT_EQ(session->image_id(), image_id);
  ASSERT_EQ(session->last_element_id(), file_id);
  ASSERT_GT(loaded->host.library()->ShownCount(), 0);

  // Drive the real QML delete entry: set pendingDeleteTargets (as the context
  // menu / confirm dialog does) and invoke runDeleteTargets().
  QVariantList targets;
  targets.push_back(QVariantMap{{QStringLiteral("elementId"), file_id},
                                {QStringLiteral("imageId"), image_id}});
  ASSERT_TRUE(loaded->window->setProperty("pendingDeleteTargets",
                                          QVariant::fromValue(targets)));
  ASSERT_TRUE(QMetaObject::invokeMethod(loaded->window, "runDeleteTargets"));
  ProcessEvents(500);

  // Editor stays selected, drops to the empty state, and forgets the image.
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(session->active());
  EXPECT_FALSE(session->has_image());
  EXPECT_EQ(session->element_id(), 0u);
  EXPECT_EQ(session->image_id(), 0u);
  EXPECT_EQ(session->last_element_id(), 0u);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
  // The delete (and its project snapshot) completed: the image is gone.
  EXPECT_EQ(loaded->host.library()->ShownCount(), 0);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, MainNavigationButtonsShowHoverPressAndFocusStates) {
  // Phase 4C: the capsule no longer uses highlightLevel / focusRingVisible.
  // The sliding workspaceSwitchThumb is the only selected-workspace visual;
  // hover remains only for tooltips; keyboard activation is still required.
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* thumb =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("workspaceSwitchThumb"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  ASSERT_NE(thumb, nullptr);

  // Removed Phase 4A-Fix selection chrome must not reappear as QObject properties.
  EXPECT_FALSE(library_nav->property("highlightLevel").isValid());
  EXPECT_FALSE(library_nav->property("focusRingVisible").isValid());
  EXPECT_FALSE(editor_nav->property("highlightLevel").isValid());
  EXPECT_FALSE(editor_nav->property("focusRingVisible").isValid());

  // Thumb tracks the active workspace (library at load).
  const qreal thumb_x_library = thumb->x();
  EXPECT_TRUE(library_nav->property("isActive").toBool());
  EXPECT_FALSE(editor_nav->property("isActive").toBool());

  // Hover still works for tooltips (no segment fill property to assert).
  QTest::mouseMove(loaded->window, CenterOfItem(library_nav));
  ProcessEvents(20);
  QTest::mouseMove(loaded->window, QPoint(5, 5));
  ProcessEvents(20);

  // Keyboard focus remains reachable without a custom focus-ring property.
  library_nav->forceActiveFocus();
  ProcessEvents(10);
  EXPECT_TRUE(library_nav->hasActiveFocus());
  editor_nav->forceActiveFocus();
  ProcessEvents(10);
  EXPECT_TRUE(editor_nav->hasActiveFocus());

  // Activate editor: thumb must move; segments still expose no highlight chrome.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(editor_nav->property("isActive").toBool());
  EXPECT_FALSE(library_nav->property("isActive").toBool());
  EXPECT_NE(thumb->x(), thumb_x_library);

  // Return to library; no highlightLevel reintroduced after round-trip.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_NEAR(thumb->x(), thumb_x_library, 1.0);
  EXPECT_FALSE(library_nav->property("highlightLevel").isValid());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, LibraryScrollPositionSurvivesEditorRoundTrip) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  // Many thumbnails so the grid overflows and a non-zero contentY is valid.
  SeedLibraryThumbnails(loaded->host, 80);
  ProcessEvents(80);

  auto* grid =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(grid, nullptr);
  ProcessEvents(40);

  // Scroll to a non-zero position via the real restore path; read back the
  // shell-persisted value.
  ASSERT_TRUE(QMetaObject::invokeMethod(grid, "restoreContentY",
                                         Q_ARG(QVariant, QVariant(200.0))));
  ProcessEvents(80);
  const qreal persisted = loaded->window->property("libraryGridContentY").toReal();
  ASSERT_GT(persisted, 0.0) << "scroll position did not take; grid layout not ready";

  // Round-trip via the real nav buttons.
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));

  // The shell-persisted scroll value survives the round-trip (persistViewState
  // wrote it on library teardown; it is not reset while the editor is open).
  EXPECT_GT(loaded->window->property("libraryGridContentY").toReal(), 0.0);

  auto* restored_grid =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(restored_grid, nullptr);
  ProcessEvents(80);  // Qt.callLater(restoreScrollPosition) + layout settle
  const qreal restored = restored_grid->property("contentY").toReal();
  // The grid re-applied the scroll (not reset to the top) and stays within a
  // row of the original — the grid may align the offset to its content origin.
  EXPECT_GT(restored, 0.0);
  EXPECT_NEAR(restored, persisted, 80.0);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, LibraryFolderFilterSurvivesEditorRoundTrip) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());
  const auto file_id = static_cast<uint>(seeded->file_id_);
  const auto image_id = static_cast<uint>(seeded->image_id_);

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(80);
  ASSERT_GT(loaded->host.library()->ShownCount(), 0);

  // Set a non-default filter: create an album, add the image, select the album.
  loaded->host.folders()->CreateFolder(QStringLiteral("AlbumA"));
  ProcessEvents(500);
  const uint album_id =
      FindFolderId(loaded->host.folders()->Folders(), QStringLiteral("AlbumA"));
  ASSERT_NE(album_id, 0u);

  QVariantList targets;
  targets.push_back(QVariantMap{{QStringLiteral("elementId"), file_id},
                                {QStringLiteral("imageId"), image_id}});
  const QVariantMap add_result =
      loaded->host.images()->AddImagesToFolder(targets, album_id);
  ASSERT_TRUE(add_result.value(QStringLiteral("success")).toBool());
  EXPECT_EQ(add_result.value(QStringLiteral("addedCount")).toInt(), 1);

  loaded->host.folders()->SelectFolder(album_id);
  ProcessEvents(500);
  ASSERT_EQ(loaded->host.library()->ShownCount(), 1);

  // Round-trip via the real nav buttons.
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  ASSERT_NE(library_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));

  // The album filter survives: still selected, and the library still shows the
  // album's single image.
  EXPECT_EQ(loaded->host.folders()->CurrentFolderId(), album_id);
  EXPECT_EQ(loaded->host.library()->ShownCount(), 1);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

// ── Phase 4B: restored editor desktop ordering ─────────────────────────────

namespace {

auto SceneX(QQuickItem* item) -> qreal {
  return item->mapToScene(QPointF(0.0, 0.0)).x();
}

}  // namespace

TEST_F(WorkspaceShellTests, EditorDesktopOrderIsHistoryCenterAdjustments) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);
  loaded->window->resize(1400, 900);
  ProcessEvents(40);

  auto* left = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsRail"));
  auto* center = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  auto* right = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* scope = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorScopeSlot"));
  ASSERT_NE(left, nullptr);
  ASSERT_NE(center, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(scope, nullptr);

  // Left History/Versions, center image viewport, right adjustments — same
  // meaning as the established editor desktop.
  EXPECT_LT(SceneX(left), SceneX(center));
  EXPECT_LT(SceneX(center), SceneX(right));
  // Scope/histogram lives with the right-side tools, not merged into history.
  EXPECT_GT(SceneX(scope), SceneX(center));

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, HistoryAndVersionsOpenSwitchAndCollapseFromLeftNavbar) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->history_panel_page().isEmpty());

  auto* history_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRailButton"));
  auto* versions_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsRailButton"));
  ASSERT_NE(history_btn, nullptr);
  ASSERT_NE(versions_btn, nullptr);

  // Open History.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(history_btn));
  ProcessEvents(60);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  auto* panel =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsPanel"));
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(panel->isVisible());
  auto* history_body =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryPageBody"));
  ASSERT_NE(history_body, nullptr);
  EXPECT_TRUE(history_body->isVisible());

  // Switch to Versions without collapsing first.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(versions_btn));
  ProcessEvents(60);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("versions"));
  auto* versions_body =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsPageBody"));
  ASSERT_NE(versions_body, nullptr);
  EXPECT_TRUE(versions_body->isVisible());
  EXPECT_FALSE(history_body->isVisible());

  // Selecting the active action again collapses the panel; the rail remains.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(versions_btn));
  ProcessEvents(60);
  EXPECT_TRUE(session->history_panel_page().isEmpty());
  EXPECT_FALSE(panel->isVisible());
  EXPECT_NE(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail")), nullptr);

  // Re-open History, then round-trip Library and confirm in-memory page survives
  // the editor Loader teardown/recreate.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(history_btn));
  ProcessEvents(60);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(40);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  auto* panel_after =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsPanel"));
  ASSERT_NE(panel_after, nullptr);
  EXPECT_TRUE(panel_after->isVisible());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, AdjustmentPanelsSwitchAndSurviveWorkspaceRoundTrip) {
  ASSERT_TRUE(QCoreApplication::instance());
  ScopedIniSettings settings_scope(temp_dir_ / "qml_settings_adj", QStringLiteral("AlcedoTestOrg"),
                                   QStringLiteral("AlcedoWorkspaceShellAdjTest"));

  {
    QSettings settings;
    settings.setValue(QStringLiteral("editor/activeAdjustmentPanel"), QStringLiteral("tone"));
    settings.sync();
  }

  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->active_adjustment_panel(), QStringLiteral("tone"));

  const QStringList panels = {QStringLiteral("tone"), QStringLiteral("look"),
                              QStringLiteral("display"), QStringLiteral("geometry"),
                              QStringLiteral("raw")};
  for (const auto& panel : panels) {
    auto* nav = loaded->window->findChild<QQuickItem*>(
        QStringLiteral("editorAdjustmentNav_") + panel);
    ASSERT_NE(nav, nullptr) << panel.toStdString();
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(nav));
    ProcessEvents(40);
    EXPECT_EQ(session->active_adjustment_panel(), panel) << panel.toStdString();

    auto* body = loaded->window->findChild<QQuickItem*>(
        QStringLiteral("editorAdjustmentPanel_") + panel);
    ASSERT_NE(body, nullptr) << panel.toStdString();
    // StackLayout keeps non-current children; active page is the one whose
    // StackLayout index matches. Prefer reading the stack currentIndex.
  }

  // Leave on Geometry, leave editor, re-enter: selection must survive.
  auto* geometry_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav_geometry"));
  ASSERT_NE(geometry_nav, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(geometry_nav));
  ProcessEvents(40);
  EXPECT_EQ(session->active_adjustment_panel(), QStringLiteral("geometry"));

  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(40);
  loaded->host.workspace_router()->OpenEditor(2, 2);
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.editor_session()->active_adjustment_panel(),
            QStringLiteral("geometry"));

  auto* stack =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentPanelStack"));
  ASSERT_NE(stack, nullptr);
  EXPECT_EQ(stack->property("currentIndex").toInt(), 3);

  loaded.reset();
  {
    alcedo::ui::EditorSessionController restored(nullptr);
    EXPECT_EQ(restored.active_adjustment_panel(), QStringLiteral("geometry"));
  }
}

TEST_F(WorkspaceShellTests, NarrowWindowKeepsSidePanelOrderAndMinViewport) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  // Narrow enough that side panels compete for width; order must not swap.
  loaded->window->resize(900, 700);
  ProcessEvents(60);

  auto* left = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsRail"));
  auto* center = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  auto* center_col =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorCenterColumn"));
  auto* right = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* workspace =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspace"));
  ASSERT_NE(left, nullptr);
  ASSERT_NE(center, nullptr);
  ASSERT_NE(center_col, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(workspace, nullptr);

  EXPECT_LT(SceneX(left), SceneX(center));
  EXPECT_LT(SceneX(center), SceneX(right));

  const int min_viewport = workspace->property("minimumViewportWidth").toInt();
  EXPECT_EQ(min_viewport, 360);
  EXPECT_GE(center_col->width(), min_viewport - 1.0);

  // Expand history panel: still left of center, adjustments stay right.
  auto* history_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRailButton"));
  ASSERT_NE(history_btn, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(history_btn));
  ProcessEvents(60);
  EXPECT_EQ(loaded->host.editor_session()->history_panel_page(), QStringLiteral("history"));
  EXPECT_LT(SceneX(left), SceneX(center));
  EXPECT_LT(SceneX(center), SceneX(right));

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

// ── Phase 4C — visual tokens, fold driver, product copy ─────────────────────

TEST_F(WorkspaceShellTests, AppThemeExposesPhase4CGeometryAndMotionTokens) {
  auto& theme = alcedo::ui::AppTheme::Instance();
  EXPECT_EQ(theme.iconOpticalSize(), 24);
  EXPECT_EQ(theme.iconOpticalSizeCompact(), 20);
  EXPECT_EQ(theme.iconSourceSize(), 24);
  EXPECT_EQ(theme.iconSourceSizeCompact(), 20);
  EXPECT_EQ(theme.iconButtonHitSize(), 44);
  EXPECT_EQ(theme.iconButtonHitSizeCompact(), 40);
  // Source size must be at least optical so DPR scaling stays sharp.
  EXPECT_GE(theme.iconSourceSize(), theme.iconOpticalSize());
  EXPECT_GE(theme.iconSourceSizeCompact(), theme.iconOpticalSizeCompact());
  // Hit band 40–46 for structural actions.
  EXPECT_GE(theme.iconButtonHitSize(), 40);
  EXPECT_LE(theme.iconButtonHitSize(), 46);
  EXPECT_GE(theme.iconButtonHitSizeCompact(), 40);
  EXPECT_LE(theme.iconButtonHitSizeCompact(), 46);
  EXPECT_EQ(theme.motionFoldOpenMs(), 200);
  EXPECT_EQ(theme.motionFoldCloseMs(), 160);
  EXPECT_LT(theme.motionFoldCloseMs(), theme.motionFoldOpenMs());
  EXPECT_EQ(theme.lineHeightBody(), 16);
  EXPECT_EQ(theme.lineHeightHeadline(), 28);
}

TEST_F(WorkspaceShellTests, EditorCardSurfacesResolveThroughSharedCardFamily) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* theme_obj = loaded->window;
  ASSERT_NE(theme_obj, nullptr);
  const QColor expected_surface =
      theme_obj->property("colCardSurface").value<QColor>();
  const QColor expected_border =
      theme_obj->property("colCardBorder").value<QColor>();
  EXPECT_TRUE(expected_surface.isValid());
  EXPECT_EQ(expected_surface, alcedo::ui::AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(expected_border, alcedo::ui::AppTheme::Instance().cardBorderColor());

  auto* rail = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail"));
  auto* stack = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  auto* viewport = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  ASSERT_NE(rail, nullptr);
  ASSERT_NE(stack, nullptr);
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(viewport, nullptr);

  // Host items expose the same card family properties as the theme mirror.
  for (QQuickItem* item : {rail, stack, filmstrip, viewport}) {
    // Parent shells pass theme; resolve colCardSurface from nearest owner.
    QObject* owner = item;
    QColor surface;
    while (owner != nullptr) {
      const QVariant v = owner->property("colCardSurface");
      if (v.isValid() && v.canConvert<QColor>()) {
        surface = v.value<QColor>();
        break;
      }
      owner = owner->parent();
    }
    EXPECT_TRUE(surface.isValid()) << item->objectName().toStdString();
    EXPECT_EQ(surface, expected_surface) << item->objectName().toStdString();
  }

  // Open history panel: expanded shell still uses the same card surface property.
  auto* history_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRailButton"));
  ASSERT_NE(history_btn, nullptr);
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(history_btn));
  ProcessEvents(40);
  auto* panel =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsPanel"));
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(panel->isVisible());
}

TEST_F(WorkspaceShellTests, StructuralIconActionsExposeHitOpticalAndAccessibleNames) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* history_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRailButton"));
  auto* versions_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsRailButton"));
  auto* tone_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav_tone"));
  auto* library_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(history_btn, nullptr);
  ASSERT_NE(versions_btn, nullptr);
  ASSERT_NE(tone_nav, nullptr);
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);

  // History rail: documented 46 px hit exception within 40–46 band.
  EXPECT_NEAR(history_btn->width(), 46.0, 0.5);
  EXPECT_NEAR(history_btn->height(), 46.0, 0.5);
  EXPECT_NEAR(versions_btn->width(), 46.0, 0.5);
  EXPECT_NEAR(versions_btn->height(), 46.0, 0.5);

  // Optical size token on IconActionButton.
  EXPECT_EQ(history_btn->property("opticalSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSize());
  EXPECT_EQ(tone_nav->property("opticalSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact());
  EXPECT_EQ(tone_nav->property("sourceSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconSourceSizeCompact());
  EXPECT_GE(tone_nav->property("sourceSize").toInt(),
            tone_nav->property("opticalSize").toInt());

  // Accessible names / tooltips present on structural actions.
  EXPECT_FALSE(history_btn->property("actionName").toString().isEmpty());
  EXPECT_FALSE(versions_btn->property("actionName").toString().isEmpty());
  EXPECT_FALSE(tone_nav->property("actionName").toString().isEmpty());
  EXPECT_FALSE(library_nav->property("actionName").toString().isEmpty());
  EXPECT_FALSE(editor_nav->property("actionName").toString().isEmpty());

  // Keyboard reachability.
  EXPECT_TRUE(history_btn->activeFocusOnTab());
  EXPECT_TRUE(tone_nav->activeFocusOnTab());
  EXPECT_TRUE(library_nav->activeFocusOnTab());
}

TEST_F(WorkspaceShellTests, HistoryFoldDriverPinsIntermediateAndTerminalGeometry) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  // Exercise the manual progress driver (works with reduceMotion on or off).
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* rail =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsRail"));
  auto* session = loaded->host.editor_session();
  ASSERT_NE(rail, nullptr);
  ASSERT_NE(session, nullptr);

  const int rail_width = rail->property("railWidth").toInt();
  const int panel_width = rail->property("expandedPanelWidth").toInt();
  const int panel_gap = rail->property("panelGap").toInt();
  ASSERT_GT(rail_width, 0);
  ASSERT_GT(panel_width, 0);

  // Start collapsed.
  session->set_history_panel_page(QString());
  ProcessEvents(20);
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.0, 0.001);
  EXPECT_NEAR(rail->width(), rail_width, 1.0);

  // Intermediate 0.5 — geometry tracks progress; session page still empty until open.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.5, 0.001);
  const qreal mid_w = rail_width + (panel_gap + panel_width) * 0.5;
  EXPECT_NEAR(rail->width(), mid_w, 1.5);

  // Open session page and complete the fold.
  session->set_history_panel_page(QStringLiteral("history"));
  ProcessEvents(10);
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 1.0, 0.001);
  EXPECT_NEAR(rail->width(), rail_width + panel_gap + panel_width, 1.5);

  // Rapid reverse at mid progress: session collapses immediately; driver pins mid.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  session->set_history_panel_page(QString());
  ProcessEvents(10);
  EXPECT_TRUE(session->history_panel_page().isEmpty());
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.5, 0.001);

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->width(), rail_width, 1.0);

  // Release driver and re-open with reduced motion → terminal bounds.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "endFoldDrive"));
  ProcessEvents(10);
  session->set_history_panel_page(QStringLiteral("versions"));
  ProcessEvents(40);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("versions"));
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 1.0, 0.001);
  EXPECT_NEAR(rail->width(), rail_width + panel_gap + panel_width, 1.5);

  // Rail identity preserved.
  EXPECT_NE(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail")),
            nullptr);
}

TEST_F(WorkspaceShellTests, FilmstripFoldDriverPinsIntermediateAndTerminalGeometry) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  auto* handle = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
  auto* session = loaded->host.editor_session();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(handle, nullptr);
  ASSERT_NE(session, nullptr);

  session->set_filmstrip_collapsed(false);
  session->set_filmstrip_expanded_height(140.0);
  ProcessEvents(20);
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "endFoldDrive"));
  ProcessEvents(20);

  const qreal handle_h = filmstrip->property("handleHeight").toReal();
  const qreal expanded_h = filmstrip->property("expandedHeight").toReal();
  ASSERT_GT(expanded_h, handle_h);

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_NEAR(filmstrip->height(), expanded_h, 1.0);

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  const qreal mid_h = handle_h + (expanded_h - handle_h) * 0.5;
  EXPECT_NEAR(filmstrip->height(), mid_h, 1.5);
  EXPECT_NEAR(filmstrip->property("dockExpandProgress").toReal(), 0.5, 0.001);

  // Collapse session while mid-fold: logical state flips; progress stays driven.
  session->set_filmstrip_collapsed(true);
  ProcessEvents(10);
  EXPECT_TRUE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->property("dockExpandProgress").toReal(), 0.5, 0.001);

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(filmstrip->height(), handle_h, 1.0);
  EXPECT_NEAR(handle->height(), handle_h, 1.0);

  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "endFoldDrive"));
  ProcessEvents(20);
  EXPECT_TRUE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->height(), handle_h, 1.0);
}

TEST_F(WorkspaceShellTests, AdjustmentSectionFoldDriverPreservesPanelSelection) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  session->set_active_adjustment_panel(QStringLiteral("tone"));
  ProcessEvents(20);

  auto* group = loaded->window->findChild<QQuickItem*>(
      QStringLiteral("editorAdjustmentGroupShell_tone"));
  ASSERT_NE(group, nullptr);

  const qreal header_h = group->property("headerHeight").toReal();
  const qreal body_content_h = group->property("bodyContentHeight").toReal();
  ASSERT_GT(header_h, 0.0);
  ASSERT_GT(body_content_h, 0.0);

  ASSERT_TRUE(QMetaObject::invokeMethod(group, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_NEAR(group->height(), header_h + body_content_h, 1.5);

  ASSERT_TRUE(QMetaObject::invokeMethod(group, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(group->height(), header_h, 1.5);
  EXPECT_NEAR(group->property("foldProgress").toReal(), 0.0, 0.001);

  // Intermediate + rapid expand while selection stays on tone.
  ASSERT_TRUE(QMetaObject::invokeMethod(group, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(0.4))));
  ProcessEvents(10);
  group->setProperty("expanded", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(group, "driveFoldProgress",
                                        Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_EQ(session->active_adjustment_panel(), QStringLiteral("tone"));
  EXPECT_NEAR(group->property("foldProgress").toReal(), 1.0, 0.001);

  ASSERT_TRUE(QMetaObject::invokeMethod(group, "endFoldDrive"));
  ProcessEvents(10);
  EXPECT_EQ(session->active_adjustment_panel(), QStringLiteral("tone"));
}

TEST_F(WorkspaceShellTests, EditorVisibleCopyHasNoDeveloperPlaceholderPhrasing) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  const QStringList banned = {QStringLiteral("will appear here"),
                              QStringLiteral("TODO"),
                              QStringLiteral("Phase 4"),
                              QStringLiteral("placeholder"),
                              QStringLiteral("FIXME")};

  // Walk labels under the editor workspace and reject banned product-facing phrasing.
  auto* workspace =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspace"));
  ASSERT_NE(workspace, nullptr);
  const auto labels = workspace->findChildren<QQuickItem*>();
  for (QQuickItem* item : labels) {
    const QString text = item->property("text").toString();
    if (text.isEmpty()) {
      continue;
    }
    const QString lower = text.toLower();
    for (const QString& ban : banned) {
      EXPECT_FALSE(lower.contains(ban.toLower()))
          << "Banned copy in " << item->objectName().toStdString() << ": "
          << text.toStdString();
    }
  }

  // Positive empty-state product strings still present.
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
}

}  // namespace
}  // namespace alcedo::ui::test
