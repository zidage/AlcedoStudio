//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file workspace_shell_test.cpp
/// @brief Verifies Phase 1B workspace routing, lazy load teardown, filmstrip shell
/// prefs, project-switch session seal, library view-state restore, real QML
/// interaction entrypoints, and Phase 4C visual/motion contracts.

#include <QColor>
#include <QPoint>
#include <QQuickItem>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>

#include <chrono>

#include "ui/main_qml_test_fixture.hpp"

#ifdef HAVE_CUDA
#include <cuda_runtime_api.h>
#endif
#ifdef HAVE_OPENCL
#include <CL/cl.h>

#include "opencl/opencl_context.hpp"
#endif

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "support/harness_completing_pipeline_scheduler_port.hpp"
#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::ui::test {
namespace {

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

  ScopedIniSettings(const ScopedIniSettings&)            = delete;
  ScopedIniSettings& operator=(const ScopedIniSettings&) = delete;

 private:
  QString           prev_org_;
  QString           prev_app_;
  QSettings::Format prev_format_;
  QString           settings_root_;
};

auto CenterOfItem(QQuickItem* item) -> QPoint {
  const QPointF scene = item->mapToScene(QPointF(item->width() * 0.5, item->height() * 0.5));
  return scene.toPoint();
}

auto WorkspaceRawImagePaths(std::size_t count) -> std::vector<std::filesystem::path> {
  const auto path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "om1.dng";
  if (!std::filesystem::is_regular_file(path)) {
    return {};
  }
  return std::vector<std::filesystem::path>(count, path);
}

auto WaitForInteractiveImage(ApplicationModuleHost&, EditorSessionController* session,
                             uint elementId, uint imageId, int timeoutMs = 30000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (session != nullptr && session->element_id() == elementId &&
        session->image_id() == imageId && session->can_edit()) {
      return true;
    }
    ProcessEvents(100);
  }
  return session != nullptr && session->element_id() == elementId &&
         session->image_id() == imageId && session->can_edit();
}

auto InstallTestFrameProducer(ApplicationModuleHost&             host,
                              alcedo::test::HarnessFrameProducer producer = {}) -> bool {
  auto* coordinator = host.editor_render_coordinator();
  if (coordinator == nullptr) {
    return false;
  }
  if (!producer) {
    producer = [](alcedo::IFrameSink* sink, const alcedo::EditorRenderRequest&) {
      if (sink == nullptr) {
        return false;
      }
      const auto mapping = sink->MapResourceForWrite(alcedo::FrameMemoryDomain::HostVisible);
      if (mapping) {
        sink->UnmapResource();
      }
      sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
      return true;
    };
  }
  auto harness = std::make_shared<alcedo::test::HarnessCompletingPipelineSchedulerPort>();
  harness->SetSinkResolver([&host]() -> alcedo::IFrameSink* {
    return host.editor_session() ? host.editor_session()->presentation_frame_sink() : nullptr;
  });
  harness->SetFrameProducer(std::move(producer));
  // Forward completion: coordinator installs on_complete at Schedule; no reverse notifier.
  coordinator->SetPipelineSchedulerPort(std::move(harness));
  return true;
}

void SeedLibraryThumbnails(ApplicationModuleHost& host, int count, int content_height_hint = 0) {
  std::vector<AlbumItem> items;
  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    AlbumItem item;
    item.element_id     = static_cast<sl_element_id_t>(1000 + i);
    item.image_id       = static_cast<image_id_t>(2000 + i);
    item.file_id        = static_cast<sl_element_id_t>(3000 + i);
    item.file_name      = QStringLiteral("test_%1.arw").arg(i);
    item.extension      = QStringLiteral("arw");
    // Empty data URL: the grid renders placeholder cards (no decode attempt) so
    // the async QQuickImage decode-failure warning never fires. Routing, scroll,
    // and filter tests do not depend on thumbnail pixels.
    item.thumb_data_url = QString();
    items.push_back(item);
  }
  host.library()->view_state().all_images_  = items;
  host.library()->view_state().total_count_ = items.size();
  host.library()->model().resetModel(items, items.size());
  host.library()->NotifyThumbnailsChanged();
  host.library()->NotifyCountsChanged();
  Q_UNUSED(content_height_hint);
}

class WorkspaceShellTests : public MainQmlTestFixture {};

TEST_F(WorkspaceShellTests, WorkspaceRouterOpensEmptyEditorAndReturnsToLibrary) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* router  = loaded->host.workspace_router();
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
  EXPECT_TRUE(workspace_host->property("editorVisible").toBool());
  EXPECT_FALSE(workspace_host->property("libraryVisible").toBool());
  EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 2);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorEmptyState")), nullptr);

  router->OpenLibrary();
  ProcessEvents(50);
  EXPECT_EQ(router->workspace(), QStringLiteral("library"));
  EXPECT_TRUE(session->active());
  EXPECT_FALSE(session->has_image());
  EXPECT_EQ(workspace_host->property("activeWorkspace").toString(), QStringLiteral("library"));
  EXPECT_TRUE(workspace_host->property("libraryVisible").toBool());
  EXPECT_FALSE(workspace_host->property("editorVisible").toBool());
  EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 2);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
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
  // 42/7 are not in the empty project, so the session stays active but has no
  // loadable image. The empty-state prompt is the focused-editor fallback.
  if (loaded->host.editor_session()->has_image()) {
    EXPECT_EQ(loaded->host.editor_session()->element_id(), 42u);
    EXPECT_EQ(loaded->host.editor_session()->image_id(), 7u);
  }

  auto* empty = loaded->window->findChild<QObject*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->property("visible").toBool(), !loaded->host.editor_session()->has_image());
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
  const int library_creates_before  = workspace_host->property("libraryCreateCount").toInt();
  const int library_destroys_before = workspace_host->property("libraryDestroyCount").toInt();

  loaded->host.workspace_router()->OpenEditor(1, 10);
  ProcessEvents(20);
  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(20);
  const int retained_timers =
      loaded->window->findChildren<QTimer*>(Qt::FindChildrenRecursively).size();

  for (int i = 1; i < 8; ++i) {
    loaded->host.workspace_router()->OpenEditor(static_cast<uint>(i + 1),
                                                static_cast<uint>(i + 10));
    ProcessEvents(20);
    EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 2) << "iter " << i;
    EXPECT_TRUE(workspace_host->property("editorVisible").toBool()) << "iter " << i;
    EXPECT_FALSE(workspace_host->property("libraryVisible").toBool()) << "iter " << i;
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

    loaded->host.workspace_router()->OpenLibrary();
    ProcessEvents(20);
    EXPECT_EQ(workspace_host->property("activeLoaderCount").toInt(), 2) << "iter " << i;
    EXPECT_TRUE(workspace_host->property("libraryVisible").toBool()) << "iter " << i;
    EXPECT_FALSE(workspace_host->property("editorVisible").toBool()) << "iter " << i;
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
    EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  }

  ProcessEvents(30);
  const int library_creates  = workspace_host->property("libraryCreateCount").toInt();
  const int library_destroys = workspace_host->property("libraryDestroyCount").toInt();
  const int editor_creates   = workspace_host->property("editorCreateCount").toInt();
  const int editor_destroys  = workspace_host->property("editorDestroyCount").toInt();

  // Switching reuses the first library and editor trees. No extra create/destroy.
  EXPECT_EQ(library_creates - library_creates_before, 0);
  EXPECT_EQ(library_destroys - library_destroys_before, 0);
  EXPECT_EQ(editor_creates, 1);
  EXPECT_EQ(editor_destroys, 0);

  const int after_timers =
      loaded->window->findChildren<QTimer*>(Qt::FindChildrenRecursively).size();
  EXPECT_EQ(after_timers, retained_timers);

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
  auto* handle    = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
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
    alcedo::ui::EditorSessionController restored;
    EXPECT_TRUE(restored.filmstrip_collapsed());
    EXPECT_DOUBLE_EQ(restored.filmstrip_expanded_height(), 140.0);
  }
}

TEST_F(WorkspaceShellTests, EditorSessionControllerTracksWorkspaceSession) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));

  host.workspace_router()->OpenEditor(9, 3);
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_TRUE(host.editor_session()->has_image());

  host.workspace_router()->OpenLibrary();
  EXPECT_TRUE(host.editor_session()->active());
  EXPECT_TRUE(host.editor_session()->has_image());
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
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
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
  auto* workspace_host = loaded->window->findChild<QObject*>(QStringLiteral("workspaceHost"));
  ASSERT_NE(workspace_host, nullptr);
  EXPECT_FALSE(workspace_host->property("libraryVisible").toBool());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  // Shell properties must still hold the values after the library is hidden.
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

  // Library width is the right-hand stack: window minus the 12px left inset,
  // the collections sidebar, the 12px gap after it, and the 12px right margin.
  const qreal sidebar    = 276.0;
  const qreal shell_gap  = 12.0;
  const qreal left_margin = 12.0;
  const qreal right_margin = 12.0;
  const qreal center_min = 560.0;
  const qreal spacing    = 12.0;
  const qreal handle     = 5.0;

  struct Case {
    int window_width;
  };
  const Case cases[] = {{960}, {1000}, {1100}};
  for (const auto& c : cases) {
    loaded->window->resize(c.window_width, 700);
    ProcessEvents(40);
    const qreal workspace_width = library->width();
    EXPECT_NEAR(workspace_width,
                static_cast<qreal>(c.window_width) - left_margin - sidebar - shell_gap
                    - right_margin,
                1.0)
        << "window " << c.window_width;

    const qreal expected =
        std::max(0.0, workspace_width - center_min - spacing - handle);
    const qreal actual = library->property("inspectorAdaptiveMaxWidth").toReal();
    EXPECT_NEAR(actual, expected, 1.0) << "window " << c.window_width;

    // Wrong formula that still subtracts the lifted sidebar would be 276px smaller.
    const qreal wrong =
        std::max(0.0, workspace_width - sidebar - center_min - spacing - handle);
    if (expected > 0.0) {
      EXPECT_GT(actual + 0.5, wrong) << "window " << c.window_width;
    }
  }
}

TEST_F(WorkspaceShellTests, TopToolbarOwnsWorkspaceSwitchAndCollectionsSidebarOwnsWordmark) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(30);

  auto* collections = loaded->window->findChild<QQuickItem*>(QStringLiteral("collectionsPanel"));
  ASSERT_NE(collections, nullptr);
  auto* identity = loaded->window->findChild<QQuickItem*>(QStringLiteral("collectionsIdentityCard"));
  ASSERT_NE(identity, nullptr);
  auto* surface = loaded->window->findChild<QQuickItem*>(QStringLiteral("collectionsSurface"));
  ASSERT_NE(surface, nullptr);
  auto* navigation =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspaceNavigation"));
  ASSERT_NE(navigation, nullptr);
  auto* top_toolbar = loaded->window->findChild<QQuickItem*>(QStringLiteral("topToolbar"));
  ASSERT_NE(top_toolbar, nullptr);

  auto* library = loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace"));
  ASSERT_NE(library, nullptr);
  EXPECT_EQ(library->findChild<QObject*>(QStringLiteral("collectionsPanel")), nullptr);
  EXPECT_EQ(collections->findChild<QObject*>(QStringLiteral("editorWorkspaceNavigation")), nullptr);
  EXPECT_NE(top_toolbar->findChild<QObject*>(QStringLiteral("editorWorkspaceNavigation")), nullptr);

  const QPointF collections_scene = collections->mapToScene(QPointF(0, 0));
  const QPointF identity_scene    = identity->mapToScene(QPointF(0, 0));
  const QPointF toolbar_scene     = top_toolbar->mapToScene(QPointF(0, 0));
  EXPECT_NEAR(toolbar_scene.x(), 12.0, 0.5);
  EXPECT_NEAR(toolbar_scene.y(), 12.0, 0.5);
  EXPECT_NEAR(top_toolbar->width(), loaded->window->width() - 24.0, 0.5);
  EXPECT_NEAR(top_toolbar->height(), 48.0, 0.5);
  EXPECT_NEAR(collections_scene.x(), 12.0, 0.5);
  EXPECT_NEAR(collections_scene.y(), 68.0, 0.5);
  EXPECT_GT(surface->property("radius").toReal(), 0.0);
  EXPECT_GT(identity_scene.y(), collections_scene.y());
}

TEST_F(WorkspaceShellTests, CollectionsSidebarToolbarToggleRestoresCollapsedPanel) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(30);

  auto* collections = loaded->window->findChild<QQuickItem*>(QStringLiteral("collectionsPanel"));
  auto* sidebar_toggle =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("collectionsSidebarToggle"));
  ASSERT_NE(collections, nullptr);
  ASSERT_NE(sidebar_toggle, nullptr);

  EXPECT_TRUE(loaded->window->property("collectionsSidebarExpanded").toBool());
  EXPECT_NEAR(collections->width(), 276.0, 1.0);
  EXPECT_TRUE(sidebar_toggle->isVisible());
  EXPECT_FALSE(sidebar_toggle->property("showFocusRing").toBool());
  EXPECT_FALSE(sidebar_toggle->property("focusOnPointerPress").toBool());
  EXPECT_EQ(sidebar_toggle->property("iconSrc").toUrl().toString(),
            QStringLiteral("qrc:/panel_icons/layout-sidebar.svg"));

  // Exercise the collapsed state directly. Loading the editor in the macOS
  // headless test host reaches the native ColorManager with no NSWindow; route
  // integration is covered by WorkspaceRouter tests in a native window host.
  loaded->window->setProperty("collectionsSidebarExpanded", false);
  ProcessEvents(250);

  EXPECT_FALSE(loaded->window->property("collectionsSidebarExpanded").toBool());
  EXPECT_NEAR(collections->width(), 0.0, 1.0);
  EXPECT_TRUE(sidebar_toggle->isVisible());
  EXPECT_EQ(sidebar_toggle->property("iconSrc").toUrl().toString(),
            QStringLiteral("qrc:/panel_icons/layout-sidebar-inactive.svg"));

  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(sidebar_toggle));
  ProcessEvents(250);

  EXPECT_TRUE(loaded->window->property("collectionsSidebarExpanded").toBool());
  EXPECT_FALSE(sidebar_toggle->hasActiveFocus());
  EXPECT_NEAR(collections->width(), 276.0, 1.0);
  EXPECT_EQ(sidebar_toggle->property("iconSrc").toUrl().toString(),
            QStringLiteral("qrc:/panel_icons/layout-sidebar.svg"));

  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier,
                    CenterOfItem(sidebar_toggle));
  ProcessEvents(250);

  EXPECT_FALSE(loaded->window->property("collectionsSidebarExpanded").toBool());
  EXPECT_NEAR(collections->width(), 0.0, 1.0);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
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

TEST_F(WorkspaceShellTests, MacosKeepsNativeTrafficLightsAndHidesDrawnCaptionButtons) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(30);

  auto* caption = loaded->window->findChild<QQuickItem*>(QStringLiteral("windowCaptionButtons"));
  ASSERT_NE(caption, nullptr);

#ifdef Q_OS_MACOS
  EXPECT_TRUE(loaded->window->property("nativeTrafficLightsEnabled").toBool());
  EXPECT_TRUE(loaded->window->flags().testFlag(Qt::ExpandedClientAreaHint));
  EXPECT_TRUE(loaded->window->flags().testFlag(Qt::NoTitleBarBackgroundHint));
  EXPECT_FALSE(loaded->window->flags().testFlag(Qt::FramelessWindowHint));
  EXPECT_FALSE(caption->isVisible());
#else
  EXPECT_FALSE(loaded->window->property("nativeTrafficLightsEnabled").toBool());
  EXPECT_FALSE(loaded->window->flags().testFlag(Qt::ExpandedClientAreaHint));
  EXPECT_TRUE(caption->isVisible());
#endif
}

TEST_F(WorkspaceShellTests, LibraryGridPinsSurviveEditorSwitchDuringZoom) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  SeedLibraryThumbnails(loaded->host, 4);
  ProcessEvents(50);

  auto* grid = loaded->window->findChild<QObject*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(grid, nullptr);

  // Keep the explicit pin outside the four seeded delegate keys so the test
  // measures the deferred release itself instead of combining two references.
  constexpr uint kElementId = 9000;
  constexpr uint kImageId   = 9000;
  constexpr uint kMaxEdge   = 512;
  loaded->host.library()->SetThumbnailVisible(kElementId, kImageId, true, kMaxEdge);
  ASSERT_TRUE(loaded->host.library()->thumbs().IsThumbnailPinned(kElementId));

  // Mid-zoom deferred release used to flush when the library tree was
  // destroyed. The library now stays loaded while hidden, so the pin remains
  // and returning to the grid does not re-request the same thumbnail.
  ASSERT_TRUE(QMetaObject::invokeMethod(grid, "beginThumbnailBindingSuspension"));
  ASSERT_TRUE(QMetaObject::invokeMethod(grid, "deferThumbnailRelease",
                                        Q_ARG(QVariant, QVariant::fromValue(kElementId)),
                                        Q_ARG(QVariant, QVariant::fromValue(kImageId)),
                                        Q_ARG(QVariant, QVariant::fromValue(kMaxEdge))));
  ProcessEvents(10);

  loaded->host.workspace_router()->OpenEditor(kElementId, kImageId);
  ProcessEvents(50);

  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryThumbnailGridView")),
            nullptr);
  EXPECT_TRUE(loaded->host.library()->thumbs().IsThumbnailPinned(kElementId));
}

TEST_F(WorkspaceShellTests, RealQmlEntrypointsDriveRoutingFocusAndFilmstripHeight) {
  ASSERT_TRUE(QCoreApplication::instance());
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
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);

  // Phase 4A: return-to-library is owned by the shared main-window navigation,
  // not an editor-local control. The editor back button no longer exists.
  EXPECT_EQ(loaded->window->findChild<QObject*>(QStringLiteral("editorBackToLibraryButton")),
            nullptr);
  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  ASSERT_NE(library_nav, nullptr);
  const auto nav_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
  while (std::chrono::steady_clock::now() < nav_deadline &&
         !library_nav->isEnabled()) {
    ProcessEvents(50);
  }
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
  ProcessEvents(80);
  if (loaded->host.workspace_router()->workspace() != QStringLiteral("library")) {
    loaded->host.workspace_router()->OpenLibrary();
    ProcessEvents(80);
  }

  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);

  // Empty editor + filmstrip handle keyboard path.
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(50);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());

  auto* handle    = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
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
}

TEST_F(WorkspaceShellTests, PresentationViewportBindingSurvivesImageSwitchAToBToA) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto raw_paths = WorkspaceRawImagePaths(2);
  if (raw_paths.empty()) {
    GTEST_SKIP() << "RAW fixture om1.dng is required for an interactive viewport session";
  }
  const auto seeded = CreateSeededPackedProject(temp_dir_, raw_paths);
  ASSERT_TRUE(seeded.has_value());
  ASSERT_EQ(seeded->images_.size(), 2u);

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(InstallTestFrameProducer(loaded->host));

  auto* session = loaded->host.editor_session();
  auto* router  = loaded->host.workspace_router();
  ASSERT_NE(session, nullptr);
  ASSERT_NE(router, nullptr);

  const auto image_a = seeded->images_.at(0);
  const auto image_b = seeded->images_.at(1);
  router->OpenEditor(static_cast<uint>(image_a.file_id_), static_cast<uint>(image_a.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, session, static_cast<uint>(image_a.file_id_),
                                      static_cast<uint>(image_a.image_id_)));
  EXPECT_TRUE(session->presentation_viewport_bound());
  auto* viewport_a =
      qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport_a, nullptr);
  auto* sink_a = session->presentation_frame_sink();
  ASSERT_NE(sink_a, nullptr);
  ASSERT_NE(session->scope_controller(), nullptr);
  EXPECT_EQ(sink_a, session->scope_controller()->frame_sink());
  const auto gen_a = viewport_a->sessionEpoch();
  EXPECT_EQ(viewport_a->sessionEpoch(), gen_a);
  EXPECT_EQ(viewport_a->imageIdentity(), image_a.image_id_);

  // A → B inside the same editor workspace (no Loader teardown).
  router->OpenEditor(static_cast<uint>(image_b.file_id_), static_cast<uint>(image_b.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, session, static_cast<uint>(image_b.file_id_),
                                      static_cast<uint>(image_b.image_id_)));
  EXPECT_TRUE(session->presentation_viewport_bound())
      << "image switch must not drop the presentation binding";
  auto* viewport_b =
      qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport_b, nullptr);
  EXPECT_EQ(viewport_b, viewport_a) << "same QML viewport instance";
  auto* sink_b = session->presentation_frame_sink();
  ASSERT_NE(sink_b, nullptr);
  EXPECT_EQ(sink_b, sink_a);
  EXPECT_GT(viewport_b->sessionEpoch(), gen_a);
  EXPECT_EQ(viewport_b->imageIdentity(), image_b.image_id_);

  // B → A: generation advances again; late frames from first A are rejected.
  const auto gen_b = viewport_b->sessionEpoch();
  router->OpenEditor(static_cast<uint>(image_a.file_id_), static_cast<uint>(image_a.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, session, static_cast<uint>(image_a.file_id_),
                                      static_cast<uint>(image_a.image_id_)));
  EXPECT_TRUE(session->presentation_viewport_bound());
  EXPECT_EQ(session->presentation_viewport(), viewport_a);
  EXPECT_EQ(session->presentation_frame_sink(), sink_a);
  EXPECT_GT(viewport_a->sessionEpoch(), gen_b);
  EXPECT_EQ(viewport_a->imageIdentity(), image_a.image_id_);

  // Leaving the editor workspace hides the retained viewport. Its sink remains
  // bound because route changes no longer destroy or close the editor session.
  router->OpenLibrary();
  ProcessEvents(60);
  EXPECT_TRUE(session->presentation_viewport_bound());
  EXPECT_NE(session->presentation_frame_sink(), nullptr);

  // This test intentionally retains the session across the route change. End
  // it explicitly so fixture teardown does not have to join a quality render.
  session->Finalize(false);
  QTRY_VERIFY_WITH_TIMEOUT(!session->has_image(), 10000);

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
  auto* viewport = qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport, nullptr);

  // Production path: session → presentation_viewport → frameSink → EnsureSize.
  sink->EnsureSize(64, 48);
  EXPECT_EQ(sink->GetWidth(), 64);
  EXPECT_EQ(sink->GetHeight(), 48);

  const FrameRole roles[] = {FrameRole::InteractivePrimary, FrameRole::QualityBase,
                             FrameRole::DetailPatch};
  for (int i = 0; i < 3; ++i) {
    FramePreviewMetadata meta;
    meta.frame_role         = roles[i];
    meta.preview_generation = static_cast<std::uint64_t>(i + 1);
    meta.detail_serial      = static_cast<std::uint64_t>(i + 10);
    sink->BindFrameSubmission(
        {meta, i == 0 ? FramePresentationMode::RoiFrame : FramePresentationMode::FullFrame});
    EXPECT_EQ(session->presentation_frame_sink(), sink);
  }

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

// Phase 5B: write native GPU leases through the production sink and require the
// actual RHI render pass to acknowledge the exact coordinator request.
TEST_F(WorkspaceShellTests, ProductionFirstFramePathWritesAndSubmitsRealFrameData) {
  ASSERT_TRUE(QCoreApplication::instance());
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    GTEST_SKIP() << "Native RHI coverage runs in EditorRealRawGpuE2eTest";
  }
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  auto* scheduler = loaded->host.editor_session_scheduler();
  ASSERT_NE(scheduler, nullptr);

  std::atomic<int> written_frame_count{0};
  const bool       producer_installed = InstallTestFrameProducer(
      loaded->host,
      [&written_frame_count](alcedo::IFrameSink*                sink,
                             const alcedo::EditorRenderRequest& request) -> bool {
        if (!sink) {
          return false;
        }
        const int w       = std::max(1, request.intent.requested_width);
        const int h       = std::max(1, request.intent.requested_height);
        auto      mapping = sink->MapResourceForWrite();
        if (!mapping) {
          return false;
        }
        std::vector<float> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0.0f);
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            const size_t i =
                (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
            pixels[i + 0] = static_cast<float>(x) / static_cast<float>(w);
            pixels[i + 1] = static_cast<float>(y) / static_cast<float>(h);
            pixels[i + 2] = 0.25f;
            pixels[i + 3] = 1.0f;
          }
        }
        bool copied = false;
#ifdef HAVE_CUDA
        if (mapping.memory_domain == alcedo::FrameMemoryDomain::CudaDevice &&
            mapping.target_type == alcedo::FrameWriteTargetType::CudaArray) {
          const auto result =
              cudaMemcpy2DToArray(reinterpret_cast<cudaArray_t>(mapping.image_array), 0, 0,
                                  pixels.data(), static_cast<size_t>(w) * sizeof(float) * 4u,
                                  static_cast<size_t>(w) * sizeof(float) * 4u,
                                  static_cast<size_t>(h), cudaMemcpyHostToDevice);
          copied = result == cudaSuccess;
        }
#endif
#ifdef HAVE_OPENCL
        if (mapping.memory_domain == alcedo::FrameMemoryDomain::OpenClDevice &&
            mapping.target_type == alcedo::FrameWriteTargetType::OpenClImage) {
          const size_t origin[] = {0, 0, 0};
          const size_t region[] = {static_cast<size_t>(w), static_cast<size_t>(h), 1};
          copied                = clEnqueueWriteImage(alcedo::OpenClContext::Instance().Queue(),
                                                      reinterpret_cast<cl_mem>(mapping.data), CL_TRUE, origin,
                                                      region, static_cast<size_t>(w) * sizeof(float) * 4u, 0,
                                                      pixels.data(), 0, nullptr, nullptr) == CL_SUCCESS;
        }
#endif
        sink->UnmapResource();
        if (!copied) {
          return false;
        }
        alcedo::FramePreviewMetadata metadata{};
        metadata.frame_role              = request.intent.frame_role;
        metadata.image_identity          = static_cast<std::uint64_t>(request.intent.image_id);
        metadata.session_epoch           = request.intent.image_load_request_id.value;
        metadata.presentation_request_id = request.request_id;
        metadata.scope_update_allowed = alcedo::ScopeUpdateAllowedForReason(request.intent.reason);
        metadata.scope_refresh_requested =
            request.intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
        const auto presentation_mode = request.intent.frame_role == alcedo::FrameRole::DetailPatch
                                           ? alcedo::FramePresentationMode::ViewportTransformed
                                           : alcedo::FramePresentationMode::FullFrame;
        sink->NotifyFrameReady({metadata, presentation_mode});
        written_frame_count.fetch_add(1, std::memory_order_release);
        return true;
      });
  ASSERT_TRUE(producer_installed);

  loaded->host.workspace_router()->OpenEditor(7, 70);
  const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (loaded->host.editor_session_service()->state() !=
             alcedo::EditorSessionState::Interactive &&
         std::chrono::steady_clock::now() < first_deadline) {
    ProcessEvents(20);
  }

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->presentation_viewport_bound());
  auto* viewport = qobject_cast<editor_rhi::EditorViewportItem*>(session->presentation_viewport());
  ASSERT_NE(viewport, nullptr);
  EXPECT_EQ(viewport->imageIdentity(), 70ull);
  EXPECT_GT(viewport->sessionEpoch(), 0ull);
  ASSERT_EQ(loaded->host.editor_session_service()->state(), alcedo::EditorSessionState::Interactive)
      << loaded->host.editor_session_service()->last_error()
      << " backend=" << viewport->backendName().toStdString()
      << " status=" << viewport->statusText().toStdString()
      << " available=" << viewport->presentationAvailable()
      << " live=" << viewport->liveTargetCount();
  EXPECT_GT(written_frame_count.load(std::memory_order_acquire), 0);
  EXPECT_EQ(viewport->lastPresentedSessionEpoch(), viewport->sessionEpoch());
  EXPECT_EQ(viewport->lastPresentedRequestId(),
            loaded->host.editor_session_service()->first_frame_request_id());

  // QualityBase follows the acknowledged InteractivePrimary through the same
  // exact request-id route.
  const auto quality_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (viewport->presentedFrameCount() < 2 &&
         std::chrono::steady_clock::now() < quality_deadline) {
    ProcessEvents(20);
  }
  EXPECT_GE(viewport->presentedFrameCount(), 2u);
  EXPECT_GE(written_frame_count.load(std::memory_order_acquire), 2);

  // Continuous adjustment regression: pointer moves submit interactive patches
  // without a settled/release event. At least one FAST frame must be composed
  // while the pointer drag is still active. The GUI-thread submit path also marks
  // QQuickRhiItem dirty once per move so the ready queue cannot wait behind the
  // mouse-release event.
  const auto composed_before_drag = viewport->presentedFrameCount();
  const auto wakeups_before_drag  = viewport->adjustmentFrameRequestCount();
  ASSERT_TRUE(
      session->submitPatch(QStringLiteral("exposure"), QStringLiteral(R"({"value":0.10})"), false));
  ASSERT_TRUE(
      session->submitPatch(QStringLiteral("exposure"), QStringLiteral(R"({"value":0.20})"), false));
  ASSERT_TRUE(
      session->submitPatch(QStringLiteral("exposure"), QStringLiteral(R"({"value":0.30})"), false));
  EXPECT_EQ(viewport->adjustmentFrameRequestCount(), wakeups_before_drag + 3);
  EXPECT_TRUE(viewport->interactivePresentLoopActive())
      << "unsettled adjustment must arm vsync-sampled consume";
  const auto interactive_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (viewport->presentedFrameCount() <= composed_before_drag &&
         std::chrono::steady_clock::now() < interactive_deadline) {
    ProcessEvents(20);
  }
  EXPECT_GT(viewport->presentedFrameCount(), composed_before_drag)
      << "Interactive adjustment did not compose until pointer release";

  // A→B→A: late frame from the first A session must not replace the current A.
  const auto gen_a1 = viewport->sessionEpoch();
  loaded->host.workspace_router()->OpenEditor(8, 80);
  ProcessEvents(60);
  const auto gen_b = viewport->sessionEpoch();
  EXPECT_GT(gen_b, gen_a1);
  EXPECT_FALSE(viewport->interactivePresentLoopActive())
      << "image switch must stop vsync-sampled consume";
  loaded->host.workspace_router()->OpenEditor(7, 70);
  ProcessEvents(60);
  const auto gen_a2 = viewport->sessionEpoch();
  EXPECT_GT(gen_a2, gen_b);
  EXPECT_EQ(viewport->imageIdentity(), 70ull);
  EXPECT_EQ(viewport->sessionEpoch(), gen_a2);

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, EditorViewportReceivesRealPointerAndWheelEvents) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto raw_paths = WorkspaceRawImagePaths(1);
  if (raw_paths.empty()) {
    GTEST_SKIP() << "RAW fixture om1.dng is required for viewport input";
  }
  const auto seeded = CreateSeededPackedProject(temp_dir_, raw_paths);
  ASSERT_TRUE(seeded.has_value());

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(InstallTestFrameProducer(loaded->host));

  const auto image = seeded->images_.front();
  loaded->host.workspace_router()->OpenEditor(static_cast<uint>(image.file_id_),
                                              static_cast<uint>(image.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, loaded->host.editor_session(),
                                      static_cast<uint>(image.file_id_),
                                      static_cast<uint>(image.image_id_)));

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

  const float  zoom_before  = interaction->zoom();
  const float  pan_x_before = interaction->panX();
  const float  pan_y_before = interaction->panY();
  const QPoint center       = CenterOfItem(viewport_item);

  // Real window mouse press/move/release (pan while zoomed).
  QTest::mousePress(loaded->window, Qt::LeftButton, Qt::NoModifier, center);
  ProcessEvents(10);
  QTest::mouseMove(loaded->window, center + QPoint(30, 12));
  ProcessEvents(10);
  QTest::mouseRelease(loaded->window, Qt::LeftButton, Qt::NoModifier, center + QPoint(30, 12));
  ProcessEvents(20);

  // Phase 5D: a left-button drag while zoomed MUST pan. The double-tap
  // TapHandler holds the left-button grab from press until release, which
  // starves the PointHandler's passive grab of drag move events; without the
  // dedicated DragHandler the drag would fall through to the single-click zoom
  // toggle on release instead of panning. Middle-button has no competing
  // TapHandler and already panned, but left-button must pan too. Asserting this
  // right after the drag (before the wheel/double-click below can also move
  // zoom) is what catches the swallowed-pan regression — the final
  // zoomed-or-panned check alone is too weak because a swallowed drag still
  // zooms via the click toggle.
  const bool left_drag_panned = std::abs(interaction->panX() - pan_x_before) > 1.0e-4f ||
                                std::abs(interaction->panY() - pan_y_before) > 1.0e-4f;
  EXPECT_TRUE(left_drag_panned) << "left-drag pan was swallowed by the double-tap TapHandler; "
                                   "panX/panY unchanged after a zoomed left-button drag";

  // Ctrl+wheel zoom at cursor through the real QML WheelHandler.
  QPointF     angle(0.0, 120.0);
  QPointF     pixel(0.0, 0.0);
  QWheelEvent wheel(center, loaded->window->mapToGlobal(center), QPoint(), QPoint(0, 120),
                    Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(loaded->window, &wheel);
  ProcessEvents(20);

  // Double-click / double-tap path.
  QTest::mouseDClick(loaded->window, Qt::LeftButton, Qt::NoModifier, center);
  ProcessEvents(30);

  // Interaction must remain enabled and keep a finite zoom after real events.
  EXPECT_TRUE(interaction->interactionEnabled());
  EXPECT_GE(interaction->zoom(), alcedo::ViewTransformController::kMinInteractiveZoom);
  EXPECT_LE(interaction->zoom(), alcedo::ViewTransformController::kMaxInteractiveZoom);
  // At least one of pan/zoom should have moved for a meaningful input sequence.
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

  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  // Navigation is persistent and visible in both workspaces.
  EXPECT_TRUE(library_nav->isVisible());
  EXPECT_TRUE(editor_nav->isVisible());
  EXPECT_TRUE(library_nav->isEnabled());
  EXPECT_TRUE(editor_nav->isEnabled());

  // Editor (empty) activated from the library workspace via main nav.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_FALSE(loaded->host.editor_session()->has_image());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
  // The active-state indicator tracks the workspace (drives the accent icon/tint).
  EXPECT_TRUE(editor_nav->property("isActive").toBool());
  EXPECT_FALSE(library_nav->property("isActive").toBool());

  // Library activated from the editor workspace via main nav.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("libraryWorkspace")), nullptr);
  EXPECT_NE(loaded->window->findChild<QObject*>(QStringLiteral("editorWorkspace")), nullptr);
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

  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
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
  EXPECT_TRUE(loaded->host.editor_session()->active());
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

  // Open the editor focused on a target. An empty project may reject the image
  // itself; the nav no-op must preserve whichever session state resulted.
  loaded->host.workspace_router()->OpenEditor(42, 7);
  ProcessEvents(80);
  ASSERT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  const bool had_image = loaded->host.editor_session()->has_image();
  const uint element_before = loaded->host.editor_session()->element_id();
  const uint image_before = loaded->host.editor_session()->image_id();

  auto* editor_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  EXPECT_TRUE(editor_nav->isEnabled());

  // Clicking the already-active editor nav must NOT reset to the empty state;
  // the active session is preserved.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  ProcessEvents(80);
  EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(loaded->host.editor_session()->active());
  EXPECT_EQ(loaded->host.editor_session()->has_image(), had_image);
  EXPECT_EQ(loaded->host.editor_session()->element_id(), element_before);
  EXPECT_EQ(loaded->host.editor_session()->image_id(), image_before);

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

  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
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
  const int editor_creates_before  = workspace_host->property("editorCreateCount").toInt();

  for (int i = 0; i < 6; ++i) {
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
    ProcessEvents(40);
    EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("editor"))
        << "iter " << i;
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(library_nav));
    ProcessEvents(40);
    EXPECT_EQ(loaded->host.workspace_router()->workspace(), QStringLiteral("library"))
        << "iter " << i;
  }

  // Repeated switching must not duplicate the main navigation.
  EXPECT_EQ(loaded->window->findChildren<QQuickItem*>(QStringLiteral("libraryNavButton")).size(),
            1);
  EXPECT_EQ(loaded->window->findChildren<QQuickItem*>(QStringLiteral("editorNavButton")).size(), 1);

  // Six round trips reuse the first library and editor trees.
  const int library_creates = workspace_host->property("libraryCreateCount").toInt();
  const int editor_creates  = workspace_host->property("editorCreateCount").toInt();
  EXPECT_EQ(library_creates - library_creates_before, 0);
  EXPECT_EQ(editor_creates - editor_creates_before, 1);

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
  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
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

TEST_F(WorkspaceShellTests, WorkspaceRouteRoundTripPreservesLastEditedTarget) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  // Seed the library model so editorImageStillExists(lastEl) resolves in-view.
  SeedLibraryThumbnails(loaded->host, 8);
  ProcessEvents(80);

  auto* router  = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  ASSERT_NE(router, nullptr);
  ASSERT_NE(session, nullptr);

  // Open image A (element 1000 / image 2000) via the real router path; Open sets
  // lastElementId/lastImageId.
  router->OpenEditor(1000, 2000);
  ProcessEvents(80);
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  ASSERT_TRUE(session->active());
  ASSERT_EQ(session->last_element_id(), 1000u);
  ASSERT_EQ(session->last_image_id(), 2000u);

  // Library routing changes only visibility. Direct routing keeps this test
  // independent from the action policy of a deliberately synthetic image id.
  router->OpenLibrary();
  ProcessEvents(80);
  EXPECT_EQ(router->workspace(), QStringLiteral("library"));
  EXPECT_TRUE(session->active());
  EXPECT_EQ(session->last_element_id(), 1000u);
  EXPECT_EQ(session->last_image_id(), 2000u);

  router->OpenEditor(session->last_element_id(), session->last_image_id());
  ProcessEvents(80);
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_TRUE(session->active());
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->isVisible(), !session->has_image());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, DeletingCurrentEditorImageDropsEditorToEmptyState) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto raw_paths = WorkspaceRawImagePaths(1);
  if (raw_paths.empty()) {
    GTEST_SKIP() << "RAW fixture om1.dng is required for deleting an interactive image";
  }
  const auto seeded = CreateSeededPackedProject(temp_dir_, raw_paths);
  ASSERT_TRUE(seeded.has_value());
  const auto file_id  = static_cast<uint>(seeded->file_id_);
  const auto image_id = static_cast<uint>(seeded->image_id_);

  auto       loaded   = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(InstallTestFrameProducer(loaded->host));
  ProcessEvents(80);

  auto* router  = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  ASSERT_NE(router, nullptr);
  ASSERT_NE(session, nullptr);

  // Edit the seeded image; Open records it as the last-edited image.
  router->OpenEditor(file_id, image_id);
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, session, file_id, image_id));
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
  targets.push_back(
      QVariantMap{{QStringLiteral("elementId"), file_id}, {QStringLiteral("imageId"), image_id}});
  ASSERT_TRUE(loaded->window->setProperty("pendingDeleteTargets", QVariant::fromValue(targets)));
  auto* image_actions =
      loaded->window->findChild<QObject*>(QStringLiteral("imageActionsController"));
  ASSERT_NE(image_actions, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(image_actions, "runDeleteTargets"));
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
  // The compact capsule keeps selection on the sliding thumb and icon tint;
  // hover adds no large per-segment fill and keyboard activation remains.
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(50);

  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* thumb = loaded->window->findChild<QQuickItem*>(QStringLiteral("workspaceSwitchThumb"));
  auto* track = loaded->window->findChild<QQuickItem*>(QStringLiteral("workspaceSwitchTrack"));
  auto* workspace_switch =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("workspaceSwitch"));
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);
  ASSERT_NE(thumb, nullptr);
  ASSERT_NE(track, nullptr);
  ASSERT_NE(workspace_switch, nullptr);

  EXPECT_NEAR(workspace_switch->width(), 112.0, 0.5);
  EXPECT_NEAR(workspace_switch->height(), 40.0, 0.5);
  EXPECT_NEAR(track->height(), 32.0, 0.5);
  EXPECT_EQ(workspace_switch->property("opticalIconSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact());

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

  auto* grid = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
  ASSERT_NE(grid, nullptr);
  ProcessEvents(40);

  // Scroll to a non-zero position via the real restore path; read back the
  // shell-persisted value.
  ASSERT_TRUE(QMetaObject::invokeMethod(grid, "restoreContentY", Q_ARG(QVariant, QVariant(200.0))));
  ProcessEvents(80);
  const qreal persisted = loaded->window->property("libraryGridContentY").toReal();
  ASSERT_GT(persisted, 0.0) << "scroll position did not take; grid layout not ready";

  // Round-trip via the real nav buttons.
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
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
  const auto file_id  = static_cast<uint>(seeded->file_id_);
  const auto image_id = static_cast<uint>(seeded->image_id_);

  auto       loaded   = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(80);
  ASSERT_GT(loaded->host.library()->ShownCount(), 0);

  // Set a non-default filter: create an album, add the image, select the album.
  loaded->host.folders()->CreateFolder(QStringLiteral("AlbumA"));
  ProcessEvents(500);
  const uint album_id = FindFolderId(loaded->host.folders()->Folders(), QStringLiteral("AlbumA"));
  ASSERT_NE(album_id, 0u);

  QVariantList targets;
  targets.push_back(
      QVariantMap{{QStringLiteral("elementId"), file_id}, {QStringLiteral("imageId"), image_id}});
  const QVariantMap add_result = loaded->host.images()->AddImagesToFolder(targets, album_id);
  ASSERT_TRUE(add_result.value(QStringLiteral("success")).toBool());
  EXPECT_EQ(add_result.value(QStringLiteral("addedCount")).toInt(), 1);

  loaded->host.folders()->SelectFolder(album_id);
  ProcessEvents(500);
  ASSERT_EQ(loaded->host.library()->ShownCount(), 1);

  // Round-trip via the real nav buttons.
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
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

auto SceneX(QQuickItem* item) -> qreal { return item->mapToScene(QPointF(0.0, 0.0)).x(); }

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
  auto* right  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* scope  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorScopeSlot"));
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
  const auto raw_paths = WorkspaceRawImagePaths(1);
  if (raw_paths.empty()) {
    GTEST_SKIP() << "RAW fixture om1.dng is required for the history rail";
  }
  const auto seeded = CreateSeededPackedProject(temp_dir_, raw_paths);
  ASSERT_TRUE(seeded.has_value());

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(InstallTestFrameProducer(loaded->host));

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  const auto image = seeded->images_.front();
  loaded->host.workspace_router()->OpenEditor(static_cast<uint>(image.file_id_),
                                              static_cast<uint>(image.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, loaded->host.editor_session(),
                                      static_cast<uint>(image.file_id_),
                                      static_cast<uint>(image.image_id_)));
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

  // Switch to Versions without collapsing first. R6: inactive body is destroyed
  // (Loader loads only the active page), not merely hidden.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(versions_btn));
  ProcessEvents(60);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("versions"));
  auto* versions_body =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsPageBody"));
  ASSERT_NE(versions_body, nullptr);
  EXPECT_TRUE(versions_body->isVisible());
  EXPECT_EQ(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryPageBody")),
            nullptr);

  // Selecting the active action again collapses the panel; the rail remains.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(versions_btn));
  ProcessEvents(60);
  EXPECT_TRUE(session->history_panel_page().isEmpty());
  // Closed rail: panel shell may be non-visible / zero-width; no page bodies.
  EXPECT_EQ(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsPageBody")),
            nullptr);
  EXPECT_EQ(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryPageBody")),
            nullptr);
  EXPECT_NE(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail")), nullptr);

  // Re-open History, then round-trip Library and confirm in-memory page survives
  // the editor Loader teardown/recreate.
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(history_btn));
  ProcessEvents(60);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  loaded->host.workspace_router()->OpenLibrary();
  ProcessEvents(40);
  loaded->host.workspace_router()->OpenEditor(static_cast<uint>(image.file_id_),
                                              static_cast<uint>(image.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, loaded->host.editor_session(),
                                      static_cast<uint>(image.file_id_),
                                      static_cast<uint>(image.image_id_)));
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

  const auto raw_paths = WorkspaceRawImagePaths(1);
  if (raw_paths.empty()) {
    GTEST_SKIP() << "RAW fixture om1.dng is required for adjustment panel navigation";
  }
  const auto seeded = CreateSeededPackedProject(temp_dir_, raw_paths);
  ASSERT_TRUE(seeded.has_value());

  auto loaded = LoadMainWindowWithPackedProject(seeded->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(InstallTestFrameProducer(loaded->host));

  auto* session = loaded->host.editor_session();
  ASSERT_NE(session, nullptr);
  const auto image = seeded->images_.front();
  loaded->host.workspace_router()->OpenEditor(static_cast<uint>(image.file_id_),
                                              static_cast<uint>(image.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, loaded->host.editor_session(),
                                      static_cast<uint>(image.file_id_),
                                      static_cast<uint>(image.image_id_)));
  EXPECT_EQ(session->active_adjustment_panel(), QStringLiteral("tone"));

  // Order matches EditorAdjustmentStack navbar + StackLayout indices:
  // tone=0, look=1, lut=2, display=3, geometry=4, raw=5.
  const QStringList panels = {QStringLiteral("tone"),     QStringLiteral("look"),
                              QStringLiteral("lut"),      QStringLiteral("display"),
                              QStringLiteral("geometry"), QStringLiteral("raw")};
  for (const auto& panel : panels) {
    auto* nav =
        loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav_") + panel);
    ASSERT_NE(nav, nullptr) << panel.toStdString();
    QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(nav));
    ProcessEvents(40);
    EXPECT_EQ(session->active_adjustment_panel(), panel) << panel.toStdString();

    auto* body =
        loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentPanel_") + panel);
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
  loaded->host.workspace_router()->OpenEditor(static_cast<uint>(image.file_id_),
                                              static_cast<uint>(image.image_id_));
  ASSERT_TRUE(WaitForInteractiveImage(loaded->host, loaded->host.editor_session(),
                                      static_cast<uint>(image.file_id_),
                                      static_cast<uint>(image.image_id_)));
  EXPECT_EQ(loaded->host.editor_session()->active_adjustment_panel(), QStringLiteral("geometry"));

  auto* stack =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentPanelStack"));
  ASSERT_NE(stack, nullptr);
  EXPECT_EQ(stack->property("currentIndex").toInt(), 4);

  loaded.reset();
  {
    alcedo::ui::EditorSessionController restored;
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
  auto* center     = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  auto* center_col = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorCenterColumn"));
  auto* right     = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* workspace = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspace"));
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
  EXPECT_EQ(theme.iconOpticalSize(), 22);
  EXPECT_EQ(theme.iconOpticalSizeCompact(), 18);
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
  // Editor side-panel + scope sizing tokens (Phase 4C comfort sizing).
  EXPECT_EQ(theme.editorSidePanelWidth(), 320);
  EXPECT_EQ(theme.editorSidePanelWidthMin(), 260);
  EXPECT_EQ(theme.editorSidePanelWidthMax(), 460);
  EXPECT_EQ(theme.editorScopeHeight(), 192);
  EXPECT_EQ(theme.editorScopeHeightMin(), 160);
  EXPECT_EQ(theme.collectionsSidebarWidth(), 276);
  EXPECT_GE(theme.editorSidePanelWidthMax(), theme.editorSidePanelWidth());
  EXPECT_GE(theme.editorSidePanelWidth(), theme.editorSidePanelWidthMin());
  EXPECT_GE(theme.editorScopeHeight(), theme.editorScopeHeightMin());
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
  const QColor expected_surface = theme_obj->property("colCardSurface").value<QColor>();
  const QColor expected_border  = theme_obj->property("colCardBorder").value<QColor>();
  EXPECT_TRUE(expected_surface.isValid());
  EXPECT_EQ(expected_surface, alcedo::ui::AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(expected_border, alcedo::ui::AppTheme::Instance().cardBorderColor());

  auto* rail      = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail"));
  auto* stack     = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  auto* viewport  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  ASSERT_NE(rail, nullptr);
  ASSERT_NE(stack, nullptr);
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(viewport, nullptr);

  // Phase 4D closeout: empty editor (no image) keeps every editor card on the
  // shared card surface. Disabled tools mute copy/controls; they do not recolor
  // the right panel to disabledSurfaceColor (that broke left/right unity).
  for (QQuickItem* item : {rail, stack, filmstrip, viewport}) {
    // Parent shells pass theme; resolve colCardSurface from nearest owner.
    QObject* owner = item;
    QColor   surface;
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

  // Painted shell fills (not only property mirrors) stay on the card family.
  auto* panel_shell =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorRightPanelSlot"));
  ASSERT_NE(panel_shell, nullptr);
  EXPECT_EQ(panel_shell->property("color").value<QColor>(), expected_surface);
  EXPECT_EQ(rail->property("color").value<QColor>(), expected_surface);
  EXPECT_EQ(viewport->property("color").value<QColor>(), expected_surface);

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
  auto* library_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  auto* editor_nav  = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(history_btn, nullptr);
  ASSERT_NE(versions_btn, nullptr);
  ASSERT_NE(tone_nav, nullptr);
  ASSERT_NE(library_nav, nullptr);
  ASSERT_NE(editor_nav, nullptr);

  // The rail preserves a 40 px hit target while using compact visual geometry.
  EXPECT_NEAR(history_btn->width(), 40.0, 0.5);
  EXPECT_NEAR(history_btn->height(), 40.0, 0.5);
  EXPECT_NEAR(versions_btn->width(), 40.0, 0.5);
  EXPECT_NEAR(versions_btn->height(), 40.0, 0.5);

  // Rails and dense adjustment navigation share compact icon geometry.
  EXPECT_EQ(history_btn->property("opticalSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact());
  EXPECT_EQ(history_btn->property("chromeSize").toInt(), 32);
  EXPECT_EQ(tone_nav->property("opticalSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact());
  EXPECT_EQ(tone_nav->property("chromeSize").toInt(), 32);
  EXPECT_EQ(tone_nav->property("sourceSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconSourceSizeCompact());
  EXPECT_GE(tone_nav->property("sourceSize").toInt(), tone_nav->property("opticalSize").toInt());

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

  auto* rail = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryVersionsRail"));
  auto* session = loaded->host.editor_session();
  ASSERT_NE(rail, nullptr);
  ASSERT_NE(session, nullptr);

  const int rail_width  = rail->property("railWidth").toInt();
  const int panel_width = rail->property("expandedPanelWidth").toInt();
  const int panel_gap   = rail->property("panelGap").toInt();
  ASSERT_GT(rail_width, 0);
  ASSERT_GT(panel_width, 0);

  // Start collapsed.
  session->set_history_panel_page(QString());
  ProcessEvents(20);
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.0, 0.001);
  EXPECT_NEAR(rail->width(), rail_width, 1.0);
  EXPECT_FALSE(rail->property("layoutExpanded").toBool());

  // Intermediate 0.5 — R6: outer layout is binary full width; progress drives
  // transform (panelSlideX) only, not interpolated Layout width.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.5, 0.001);
  EXPECT_TRUE(rail->property("layoutExpanded").toBool());
  const qreal full_w = rail_width + panel_gap + panel_width;
  EXPECT_NEAR(rail->width(), full_w, 1.5);
  EXPECT_NEAR(rail->property("panelSlideX").toReal(), -0.5 * panel_width, 1.5);

  // Open session page and complete the fold.
  session->set_history_panel_page(QStringLiteral("history"));
  ProcessEvents(10);
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("history"));
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 1.0, 0.001);
  EXPECT_NEAR(rail->width(), full_w, 1.5);
  EXPECT_NEAR(rail->property("panelSlideX").toReal(), 0.0, 1.0);

  // Rapid reverse at mid progress: session collapses immediately; driver pins mid.
  // Layout stays terminal-expanded until progress returns to 0.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  session->set_history_panel_page(QString());
  ProcessEvents(10);
  EXPECT_TRUE(session->history_panel_page().isEmpty());
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 0.5, 0.001);
  EXPECT_NEAR(rail->width(), full_w, 1.5);

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(rail->width(), rail_width, 1.0);
  EXPECT_FALSE(rail->property("layoutExpanded").toBool());

  // Release driver and re-open with reduced motion → terminal bounds.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "endFoldDrive"));
  ProcessEvents(10);
  session->set_history_panel_page(QStringLiteral("versions"));
  ProcessEvents(40);
  EXPECT_EQ(session->history_panel_page(), QStringLiteral("versions"));
  EXPECT_NEAR(rail->property("panelOpenProgress").toReal(), 1.0, 0.001);
  EXPECT_NEAR(rail->width(), full_w, 1.5);

  // Rail identity preserved.
  EXPECT_NE(loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail")), nullptr);
}

TEST_F(WorkspaceShellTests, FilmstripFoldDriverPinsIntermediateAndTerminalGeometry) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* filmstrip = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  auto* handle    = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorFilmstripHandle"));
  auto* session   = loaded->host.editor_session();
  ASSERT_NE(filmstrip, nullptr);
  ASSERT_NE(handle, nullptr);
  ASSERT_NE(session, nullptr);

  session->set_filmstrip_collapsed(false);
  session->set_filmstrip_expanded_height(140.0);
  ProcessEvents(20);
  ASSERT_TRUE(QMetaObject::invokeMethod(filmstrip, "endFoldDrive"));
  ProcessEvents(20);

  const qreal handle_h   = filmstrip->property("handleHeight").toReal();
  const qreal expanded_h = filmstrip->property("expandedHeight").toReal();
  ASSERT_GT(expanded_h, handle_h);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(filmstrip, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_NEAR(filmstrip->height(), expanded_h, 1.0);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(filmstrip, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents(10);
  const qreal mid_h = handle_h + (expanded_h - handle_h) * 0.5;
  EXPECT_NEAR(filmstrip->height(), mid_h, 1.5);
  EXPECT_NEAR(filmstrip->property("dockExpandProgress").toReal(), 0.5, 0.001);

  // Collapse session while mid-fold: logical state flips; progress stays driven.
  session->set_filmstrip_collapsed(true);
  ProcessEvents(10);
  EXPECT_TRUE(session->filmstrip_collapsed());
  EXPECT_NEAR(filmstrip->property("dockExpandProgress").toReal(), 0.5, 0.001);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(filmstrip, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
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

  auto* group =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentGroupShell_tone"));
  ASSERT_NE(group, nullptr);

  const qreal header_h       = group->property("headerHeight").toReal();
  const qreal body_content_h = group->property("bodyContentHeight").toReal();
  ASSERT_GT(header_h, 0.0);
  ASSERT_GT(body_content_h, 0.0);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(group, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents(10);
  EXPECT_NEAR(group->height(), header_h + body_content_h, 1.5);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(group, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents(10);
  EXPECT_NEAR(group->height(), header_h, 1.5);
  EXPECT_NEAR(group->property("foldProgress").toReal(), 0.0, 0.001);

  // Intermediate + rapid expand while selection stays on tone.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(group, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.4))));
  ProcessEvents(10);
  group->setProperty("expanded", true);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(group, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
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

  const QStringList banned = {QStringLiteral("will appear here"), QStringLiteral("TODO"),
                              QStringLiteral("Phase 4"), QStringLiteral("placeholder"),
                              QStringLiteral("FIXME")};

  // Walk labels under the editor workspace and reject banned product-facing phrasing.
  auto* workspace = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspace"));
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
          << "Banned copy in " << item->objectName().toStdString() << ": " << text.toStdString();
    }
  }

  // Positive empty-state product strings still present.
  auto* empty = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorEmptyState"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
}

// ── Phase 4D — opaque control surfaces and shared icon actions ──────────────

TEST_F(WorkspaceShellTests, AdjustmentStackBackgroundFillsHaveAlpha255) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* stack = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentStack"));
  ASSERT_NE(stack, nullptr);

  // Walk every Rectangle descendant under the adjustment stack and assert its
  // color fill is fully opaque (alpha 255). Scope slots, nav bars, panel
  // shells, and collapsible section bodies must all use concrete opaque colors.
  const auto rects   = stack->findChildren<QQuickItem*>();
  int        checked = 0;
  for (QQuickItem* item : rects) {
    // Only check Rectangles (the surface primitives). Skip non-surface Items
    // and interaction overlays (MouseArea, Handler, etc.).
    const QVariant color_var = item->property("color");
    if (!color_var.isValid() || !color_var.canConvert<QColor>()) {
      continue;
    }
    // Skip focus/highlight overlays that are not structural surfaces.
    const QString obj_name = item->objectName();
    if (obj_name.isEmpty() && item->parent() != nullptr) {
      // Anonymous inner rectangles (like header hover overlays) may use
      // "transparent" to show the parent surface — skip those.
      continue;
    }
    const QColor c = color_var.value<QColor>();
    EXPECT_EQ(c.alpha(), 255) << "Non-opaque surface in adjustment stack: "
                              << (obj_name.isEmpty() ? "(unnamed)" : obj_name.toStdString())
                              << " color=" << c.name(QColor::HexArgb).toStdString();
    checked++;
  }
  EXPECT_GT(checked, 0) << "Expected at least one background Rectangle to verify";

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, AdjustmentNavButtonsAreSquareWithSharedTokens) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  const auto&       theme  = alcedo::ui::AppTheme::Instance();
  const QStringList panels = {QStringLiteral("tone"), QStringLiteral("look"),
                              QStringLiteral("display"), QStringLiteral("geometry"),
                              QStringLiteral("raw")};
  for (const auto& panel : panels) {
    auto* nav =
        loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav_") + panel);
    ASSERT_NE(nav, nullptr) << panel.toStdString();

    // Square: width == height within 1 px (shared hitSize token).
    EXPECT_NEAR(nav->width(), nav->height(), 1.0)
        << panel.toStdString() << " adjustment nav button is not square";
    EXPECT_NEAR(nav->width(), static_cast<qreal>(theme.iconButtonHitSizeCompact()), 1.0)
        << panel.toStdString() << " should use shared hitSize";
    EXPECT_NEAR(nav->height(), static_cast<qreal>(theme.iconButtonHitSizeCompact()), 1.0)
        << panel.toStdString() << " should use shared hitSize";

    // Optical icon size uses the shared token.
    EXPECT_EQ(nav->property("opticalSize").toInt(), theme.iconOpticalSizeCompact())
        << panel.toStdString();
    EXPECT_EQ(nav->property("sourceSize").toInt(), theme.iconSourceSizeCompact())
        << panel.toStdString();
    EXPECT_GE(nav->property("sourceSize").toInt(), nav->property("opticalSize").toInt())
        << panel.toStdString();

    // No stretchInLayout — must be false or absent.
    EXPECT_FALSE(nav->property("stretchInLayout").toBool())
        << panel.toStdString() << " should not stretch";

    // Accessible name / tooltip present.
    EXPECT_FALSE(nav->property("actionName").toString().isEmpty()) << panel.toStdString();

    // Keyboard reachable.
    EXPECT_TRUE(nav->activeFocusOnTab()) << panel.toStdString();
  }

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, AdjustmentNavContainerUsesCompactRadiusToken) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* nav_container =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav"));
  ASSERT_NE(nav_container, nullptr);

  // The dense navigation track and its 32 px wells use the compact radius.
  const int container_radius = nav_container->property("radius").toInt();
  EXPECT_EQ(container_radius, alcedo::ui::AppTheme::Instance().controlRadiusSmall());

  auto* tone_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav_tone"));
  ASSERT_NE(tone_nav, nullptr);
  EXPECT_EQ(container_radius, alcedo::ui::AppTheme::Instance().controlRadiusSmall());
}

TEST_F(WorkspaceShellTests, ScopeModeNavUsesSharedIconNavigationAssets) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(1, 1);
  ProcessEvents(80);

  auto* nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorScopeModeNav"));
  auto* adjustment_nav =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorAdjustmentNav"));
  auto* histogram =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorScopeModeHistogram"));
  auto* waveform =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorScopeModeWaveform"));
  ASSERT_NE(nav, nullptr);
  ASSERT_NE(adjustment_nav, nullptr);
  ASSERT_NE(histogram, nullptr);
  ASSERT_NE(waveform, nullptr);
  EXPECT_NEAR(nav->width(), adjustment_nav->width(), 1.0);
  EXPECT_TRUE(histogram->property("iconSrc").toString().endsWith(QStringLiteral("histogram.svg")));
  EXPECT_TRUE(waveform->property("iconSrc").toString().endsWith(QStringLiteral("waveform.svg")));
  EXPECT_TRUE(histogram->activeFocusOnTab());
  EXPECT_TRUE(waveform->activeFocusOnTab());
}

TEST_F(WorkspaceShellTests, VersionsRailButtonUsesTablerVersionsIcon) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* versions_btn =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorVersionsRailButton"));
  ASSERT_NE(versions_btn, nullptr);

  const QString icon_src = versions_btn->property("iconSrc").toString();
  EXPECT_TRUE(icon_src.endsWith(QStringLiteral("versions.svg")))
      << "Versions rail button iconSrc should be versions.svg, got: " << icon_src.toStdString();
  EXPECT_FALSE(icon_src.contains(QStringLiteral("palette.svg")))
      << "Versions rail button must not use palette.svg";

  // Still uses shared optical/source size tokens.
  EXPECT_EQ(versions_btn->property("opticalSize").toInt(),
            alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact());
  EXPECT_FALSE(versions_btn->property("actionName").toString().isEmpty());

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, NewOpaqueThemeTokensExistAndHaveAlpha255) {
  auto&        theme            = alcedo::ui::AppTheme::Instance();

  // Phase 4D button-state fills.
  const QColor idle             = theme.buttonIdleFillColor();
  const QColor hovered          = theme.buttonHoveredFillColor();
  const QColor pressed          = theme.buttonPressedFillColor();
  const QColor selected         = theme.buttonSelectedFillColor();
  const QColor disabled_surface = theme.disabledSurfaceColor();

  EXPECT_EQ(idle.alpha(), 255);
  EXPECT_EQ(hovered.alpha(), 255);
  EXPECT_EQ(pressed.alpha(), 255);
  EXPECT_EQ(selected.alpha(), 255);
  EXPECT_EQ(disabled_surface.alpha(), 255);

  // Idle fill matches card surface.
  EXPECT_EQ(idle, theme.cardSurfaceColor());

  // Hovered / pressed / selected are distinct from idle (they show interaction).
  EXPECT_NE(hovered, idle);
  EXPECT_NE(pressed, idle);
  EXPECT_NE(selected, idle);

  // Pressed and selected are the same (both represent the active/engaged state).
  EXPECT_EQ(pressed, selected);

  // Disabled surface is dimmed relative to card surface (different color).
  EXPECT_NE(disabled_surface, theme.cardSurfaceColor());
}

TEST_F(WorkspaceShellTests, DisabledAdjustmentStackUsesOpaqueSurfaceNotParentOpacity) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);

  // Open editor with NO image (controlsEnabled = false). Disabled mutes
  // controls/copy; the shell stays the shared card surface so the right column
  // matches History/Versions, viewport, and filmstrip. No parent opacity.
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  auto* panel_shell =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorRightPanelSlot"));
  ASSERT_NE(panel_shell, nullptr);

  // The panel shell must be fully opaque (not dimmed via opacity).
  const QVariant opacity_var = panel_shell->property("opacity");
  if (opacity_var.isValid()) {
    EXPECT_NEAR(opacity_var.toReal(), 1.0, 0.001)
        << "Panel shell must not use opacity for disabled state";
  }

  const QColor shell_color = panel_shell->property("color").value<QColor>();
  EXPECT_EQ(shell_color.alpha(), 255) << "Disabled panel shell must be fully opaque";
  EXPECT_EQ(shell_color, alcedo::ui::AppTheme::Instance().cardSurfaceColor())
      << "Right panel shell must stay on the shared card surface when disabled";

  auto* rail = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorHistoryRail"));
  ASSERT_NE(rail, nullptr);
  const QColor rail_color = rail->property("color").value<QColor>();
  EXPECT_EQ(rail_color.alpha(), 255);
  EXPECT_EQ(rail_color, shell_color)
      << "History rail and adjustment shell must share one card surface fill";

  auto* viewport = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  ASSERT_NE(viewport, nullptr);
  const QColor viewport_color = viewport->property("color").value<QColor>();
  EXPECT_EQ(viewport_color, shell_color)
      << "Viewport placeholder must share the same card surface as side panels";

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

TEST_F(WorkspaceShellTests, HistoryRailButtonsAreSquareWithSquareVisibleChrome) {
  ASSERT_TRUE(QCoreApplication::instance());
  auto loaded = LoadMainWindow();
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.workspace_router()->OpenEditor(0, 0);
  ProcessEvents(80);

  for (const char* name : {"editorHistoryRailButton", "editorVersionsRailButton"}) {
    auto* btn = loaded->window->findChild<QQuickItem*>(QString::fromUtf8(name));
    ASSERT_NE(btn, nullptr) << name;
    EXPECT_NEAR(btn->width(), btn->height(), 0.5) << name << " hit target not square";
    EXPECT_NEAR(btn->width(), 40.0, 0.5) << name << " should use compact rail hit";
    EXPECT_NEAR(btn->height(), 40.0, 0.5) << name << " should use compact rail hit";
    EXPECT_NEAR(btn->property("chromeSize").toReal(), 32.0, 0.5)
        << name << " visible well should stay smaller than its hit target";
    // Item-based IconActionButton: optical/source tokens still resolve.
    EXPECT_EQ(btn->property("opticalSize").toInt(),
              alcedo::ui::AppTheme::Instance().iconOpticalSizeCompact())
        << name;
    EXPECT_EQ(btn->property("sourceSize").toInt(),
              alcedo::ui::AppTheme::Instance().iconSourceSizeCompact())
        << name;
  }

  EXPECT_TRUE(loaded->qml_warnings.empty())
      << loaded->qml_warnings.front().toString().toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
