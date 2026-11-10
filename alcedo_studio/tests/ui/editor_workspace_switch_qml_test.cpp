//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Issue 747: the switch between the Library and the editor on the production Main.qml. On a
// switch the UI thread changes visibility and queues commands only:
// - The toolbar return to the editor runs no database read on the UI thread. The editor list
//   check reads the position of the open photo on the query worker
//   (LibraryModule::RequestEditorListPosition).
// - The hidden workspace keeps its size while the collections sidebar folds, so it does not lay
//   out on every frame of the fold (WorkspaceHost.qml).
// Issue 751: the motion of the collections sidebar.
// - A workspace switch applies the sidebar state of the new workspace at once; the shown
//   workspace takes its final width once and fades in.
// - A manual toggle slides the sidebar at its full width; the shown workspace follows the
//   sidebar edge on every frame. The thumbnail cache size waits for the end of the fold; the
//   editor viewport keeps its size, is scaled to the slot, and renders once at the end.
// Issue 754: when the fold flag drops, the shown workspace already has its final width, so
//   the Library grid sets the cache size once, for that width.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QPoint>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app/project_service.hpp"
#include "sleeve/storage.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/main_qml_test_fixture.hpp"
#include "../support/storage_test_access.hpp"

namespace alcedo::ui::test {
namespace {

using EditorWorkspaceSwitchQmlTest = MainQmlTestFixture;

auto CenterOfItem(QQuickItem* item) -> QPoint {
  return item->mapToScene(QPointF(item->width() * 0.5, item->height() * 0.5)).toPoint();
}

/// Holds the project database lock on a helper thread, as a save, an import, or a page read
/// does, until Release or the destructor. The helper releases the lock after 30 s at the
/// latest, so a read on the UI thread fails the test instead of hanging it.
class DatabaseLockHold {
 public:
  explicit DatabaseLockHold(Database& database)
      : holder_([this, &database]() {
          auto             guard   = database.GetConnectionGuard();
          auto             db_lock = guard.Lock();
          std::unique_lock lock(mutex_);
          held_ = true;
          changed_.notify_all();
          changed_.wait_for(lock, std::chrono::seconds(30), [this]() { return release_; });
        }) {}
  ~DatabaseLockHold() { Release(); }
  DatabaseLockHold(const DatabaseLockHold&)            = delete;
  DatabaseLockHold& operator=(const DatabaseLockHold&) = delete;

  auto              WaitHeld() -> bool {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(10), [this]() { return held_; });
  }
  void Release() {
    {
      std::lock_guard lock(mutex_);
      release_ = true;
    }
    changed_.notify_all();
    if (holder_.joinable()) {
      holder_.join();
    }
  }

 private:
  std::mutex              mutex_;
  std::condition_variable changed_;
  bool                    held_    = false;
  bool                    release_ = false;
  std::thread             holder_;  // Last member: starts after the state above exists.
};

/// The QML warnings of the loaded window, one per line, without the warning that Qt's Material
/// Dialog.qml reports for every Main.qml load in this fixture (it fails the existing
/// EditorCheckpointQmlIntegrationTest cases too).
auto AppQmlWarnings(const LoadedMainWindow& loaded) -> QString {
  QStringList lines;
  for (const auto& warning : loaded.qml_warnings) {
    const QString text = warning.toString();
    if (!text.contains(QStringLiteral("Controls/Material/Dialog.qml"))) {
      lines.push_back(text);
    }
  }
  return lines.join(QLatin1Char('\n'));
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs = 15000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(20);
  }
  return predicate();
}

// The open photo is outside the loaded rows (130 photos and a search, so a page holds 120
// rows), and a helper thread holds the database lock. The click on the editor button and the
// events after it complete while the lock is held; the list check answers when the lock is
// free, and the editor keeps the photo, which the list holds.
TEST_F(EditorWorkspaceSwitchQmlTest, ReturnToEditorDoesNotWaitForDatabaseLock) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto populated =
      CreatePopulatedPackedProject(temp_dir_, {}, 0, std::vector<LibraryPhotoSpec>(130));
  ASSERT_TRUE(populated.has_value());
  ASSERT_EQ(populated->images_.size(), 130u);

  auto loaded = LoadMainWindowWithPackedProject(populated->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  loaded->host.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  ASSERT_TRUE(WaitForLibraryQuery(loaded->host));
  ProcessEvents(200);
  ASSERT_EQ(loaded->host.library()->ShownCount(), 120);

  const auto target   = populated->images_[125];
  const auto file_id  = static_cast<uint>(target.file_id_);
  const auto image_id = static_cast<uint>(target.image_id_);
  ASSERT_LT(loaded->host.library()->model().rowByElementId(file_id), 0);
  auto* router  = loaded->host.workspace_router();
  auto* session = loaded->host.editor_session();
  router->OpenEditor(file_id, image_id);
  ProcessEvents(500);
  router->OpenLibrary();
  ProcessEvents(500);
  ASSERT_EQ(router->workspace(), QStringLiteral("library"));
  ASSERT_EQ(session->last_element_id(), file_id);

  DatabaseLockHold hold(
      StorageTestAccess::GetDatabase(*loaded->host.project()->handler().project()->GetStorage()));
  ASSERT_TRUE(hold.WaitHeld());

  auto* editor_nav = loaded->window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  ASSERT_NE(editor_nav, nullptr);
  QElapsedTimer timer;
  timer.start();
  QTest::mouseClick(loaded->window, Qt::LeftButton, Qt::NoModifier, CenterOfItem(editor_nav));
  const qint64 click_ms = timer.elapsed();
  ProcessEvents(300);
  const qint64 switch_ms = timer.elapsed();
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_LT(click_ms, 1000);
  EXPECT_LT(switch_ms, 3000);

  hold.Release();
  EXPECT_TRUE(WaitUntil([&]() { return router->element_id() == file_id; }));
  ProcessEvents(300);
  EXPECT_EQ(router->workspace(), QStringLiteral("editor"));
  EXPECT_EQ(router->element_id(), file_id);
  EXPECT_EQ(router->image_id(), image_id);
  EXPECT_EQ(AppQmlWarnings(*loaded), QString());
}

// The collections sidebar folds when the editor opens and expands when the Library opens. The
// hidden workspace keeps its size; the shown workspace follows the host.
TEST_F(EditorWorkspaceSwitchQmlTest, HiddenWorkspaceKeepsItsSizeWhileSidebarFolds) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 2);
  ASSERT_TRUE(populated.has_value());
  auto loaded = LoadMainWindowWithPackedProject(populated->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(200);

  auto* host = loaded->window->findChild<QQuickItem*>(QStringLiteral("workspaceHost"));
  auto* library_loader =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("libraryWorkspaceLoader"));
  auto* editor_loader =
      loaded->window->findChild<QQuickItem*>(QStringLiteral("editorWorkspaceLoader"));
  ASSERT_NE(host, nullptr);
  ASSERT_NE(library_loader, nullptr);
  ASSERT_NE(editor_loader, nullptr);
  const qreal library_width = library_loader->width();
  ASSERT_GT(library_width, 0.0);
  EXPECT_DOUBLE_EQ(library_width, host->width());

  // Every width change of the Library while it is hidden.
  int        library_width_changes = 0;
  const auto connection            = QObject::connect(library_loader, &QQuickItem::widthChanged,
                                                      [&]() { ++library_width_changes; });
  auto*      router                = loaded->host.workspace_router();
  router->OpenEditor(static_cast<uint>(populated->images_[0].file_id_),
                     static_cast<uint>(populated->images_[0].image_id_));
  ProcessEvents(800);
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  // The sidebar folded: the host is wider, the editor follows it, the Library does not.
  EXPECT_GT(host->width(), library_width);
  EXPECT_DOUBLE_EQ(editor_loader->width(), host->width());
  EXPECT_DOUBLE_EQ(library_loader->width(), library_width);
  EXPECT_EQ(library_width_changes, 0);
  QObject::disconnect(connection);

  // Shown again, the Library takes the host size, and the hidden editor keeps its own.
  const qreal editor_width = editor_loader->width();
  router->OpenLibrary();
  ProcessEvents(800);
  EXPECT_DOUBLE_EQ(library_loader->width(), host->width());
  EXPECT_DOUBLE_EQ(library_loader->width(), library_width);
  EXPECT_DOUBLE_EQ(editor_loader->width(), editor_width);
  EXPECT_EQ(AppQmlWarnings(*loaded), QString());
}

/// Turns Reduce Motion off for one test; the fixture turns it on for every load.
class MotionOn {
 public:
  MotionOn() { AppTheme::Instance().setReduceMotion(false); }
  ~MotionOn() { AppTheme::Instance().setReduceMotion(true); }
  MotionOn(const MotionOn&)            = delete;
  MotionOn& operator=(const MotionOn&) = delete;
};

auto IntProperty(QObject* object, const char* name) -> int {
  return object->property(name).toInt();
}

/// Runs a manual toggle of the collections sidebar with motion on, until the fold ends. While
/// the fold runs, @p during is called after each event pass. Returns the steps of the sidebar
/// edge (changes of collectionsSidebarReveal). The fold animation runs on its own clock: in a
/// debug build a frame can show two steps or none, so the steps are counted, not the frames.
auto RunManualSidebarToggle(QQuickWindow* window, const std::function<void()>& during) -> int {
  QSignalSpy edge_steps(window, SIGNAL(collectionsSidebarRevealChanged()));
  EXPECT_TRUE(edge_steps.isValid());
  QMetaObject::invokeMethod(window, "toggleCollectionsSidebar");
  EXPECT_TRUE(window->property("collectionsSidebarFolding").toBool());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (window->property("collectionsSidebarFolding").toBool() &&
         std::chrono::steady_clock::now() < deadline) {
    during();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  }
  EXPECT_FALSE(window->property("collectionsSidebarFolding").toBool());
  EXPECT_GE(edge_steps.count(), 1);
  return static_cast<int>(edge_steps.count());
}

// Issue 751: a workspace switch is a cut. The sidebar state of the new workspace applies at
// once, the shown workspace takes its final width once, in the same call as the route change,
// and it fades in (opacity and a short slide; its layout does not change).
TEST_F(EditorWorkspaceSwitchQmlTest, WorkspaceSwitchAppliesSidebarAtOnceAndFadesIn) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 2);
  ASSERT_TRUE(populated.has_value());
  auto loaded = LoadMainWindowWithPackedProject(populated->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(200);
  const MotionOn motion;

  auto* window         = loaded->window;
  auto* host           = window->findChild<QQuickItem*>(QStringLiteral("workspaceHost"));
  auto* library_loader = window->findChild<QQuickItem*>(QStringLiteral("libraryWorkspaceLoader"));
  auto* editor_loader  = window->findChild<QQuickItem*>(QStringLiteral("editorWorkspaceLoader"));
  ASSERT_NE(host, nullptr);
  ASSERT_NE(library_loader, nullptr);
  ASSERT_NE(editor_loader, nullptr);
  const qreal library_width = library_loader->width();
  ASSERT_DOUBLE_EQ(window->property("collectionsSidebarReveal").toDouble(), 1.0);

  int        host_width_changes   = 0;
  int        editor_width_changes = 0;
  const auto host_connection      = QObject::connect(host, &QQuickItem::widthChanged,
                                                     [&]() { ++host_width_changes; });
  const auto editor_connection    = QObject::connect(editor_loader, &QQuickItem::widthChanged,
                                                     [&]() { ++editor_width_changes; });
  auto*      router               = loaded->host.workspace_router();
  router->OpenEditor(static_cast<uint>(populated->images_[0].file_id_),
                     static_cast<uint>(populated->images_[0].image_id_));
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  // Before any event or frame: the sidebar is folded, the host and the editor have their final
  // width, and the fade has started.
  EXPECT_DOUBLE_EQ(window->property("collectionsSidebarReveal").toDouble(), 0.0);
  EXPECT_FALSE(window->property("collectionsSidebarFolding").toBool());
  EXPECT_EQ(host_width_changes, 1);
  EXPECT_EQ(editor_width_changes, 1);
  EXPECT_DOUBLE_EQ(editor_loader->width(), host->width());
  EXPECT_GT(host->width(), library_width);
  EXPECT_TRUE(host->property("revealRunning").toBool());
  EXPECT_LT(editor_loader->opacity(), 1.0);

  ProcessEvents(500);
  EXPECT_FALSE(host->property("revealRunning").toBool());
  EXPECT_DOUBLE_EQ(editor_loader->opacity(), 1.0);
  EXPECT_DOUBLE_EQ(editor_loader->x(), 0.0);
  EXPECT_EQ(host_width_changes, 1);
  EXPECT_EQ(editor_width_changes, 1);
  QObject::disconnect(host_connection);
  QObject::disconnect(editor_connection);

  // Back to the Library: the sidebar opens at once, and the Library is at the width it kept.
  int        library_width_changes = 0;
  const auto library_connection    = QObject::connect(library_loader, &QQuickItem::widthChanged,
                                                      [&]() { ++library_width_changes; });
  router->OpenLibrary();
  EXPECT_DOUBLE_EQ(window->property("collectionsSidebarReveal").toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(library_loader->width(), host->width());
  EXPECT_DOUBLE_EQ(library_loader->width(), library_width);
  EXPECT_TRUE(host->property("revealRunning").toBool());
  ProcessEvents(500);
  QObject::disconnect(library_connection);
  EXPECT_EQ(library_width_changes, 0);
  EXPECT_DOUBLE_EQ(library_loader->opacity(), 1.0);
  EXPECT_DOUBLE_EQ(library_loader->x(), 0.0);
  EXPECT_EQ(AppQmlWarnings(*loaded), QString());
}

// Issue 751: a manual toggle in the Library. The sidebar slides at its full width (its content
// does not reflow), and the Library follows the sidebar edge with one width change per frame.
// The thumbnail cache size waits for the end of the fold.
TEST_F(EditorWorkspaceSwitchQmlTest, LibraryFollowsManualSidebarToggleAndSizesCacheOnce) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 24);
  ASSERT_TRUE(populated.has_value());
  auto loaded = LoadMainWindowWithPackedProject(populated->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(300);
  const MotionOn motion;

  auto* window         = loaded->window;
  auto* host           = window->findChild<QQuickItem*>(QStringLiteral("workspaceHost"));
  auto* library_loader = window->findChild<QQuickItem*>(QStringLiteral("libraryWorkspaceLoader"));
  auto* grid           = window->findChild<QQuickItem*>(QStringLiteral("libraryThumbnailGridView"));
  auto* sidebar        = window->findChild<QQuickItem*>(QStringLiteral("collectionsSidebar"));
  ASSERT_NE(host, nullptr);
  ASSERT_NE(library_loader, nullptr);
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(sidebar, nullptr);
  const qreal open_width    = library_loader->width();
  const qreal sidebar_width = sidebar->width();
  ASSERT_GT(sidebar_width, 0.0);

  for (const bool expanded_after : {false, true}) {
    SCOPED_TRACE(expanded_after ? "open" : "fold");
    int        width_changes = 0;
    const auto connection    = QObject::connect(library_loader, &QQuickItem::widthChanged,
                                                [&]() { ++width_changes; });
    const int  hints_before  = IntProperty(grid, "cacheHintUpdateCount");
    bool       hint_in_fold  = false;
    bool       panel_resized = false;
    const int  edge_steps    = RunManualSidebarToggle(window, [&]() {
      hint_in_fold  = hint_in_fold || IntProperty(grid, "cacheHintUpdateCount") != hints_before;
      panel_resized = panel_resized || sidebar->width() != sidebar_width;
      EXPECT_DOUBLE_EQ(library_loader->width(), host->width());
    });
    QObject::disconnect(connection);
    EXPECT_EQ(window->property("collectionsSidebarExpanded").toBool(), expanded_after);
    EXPECT_FALSE(hint_in_fold);
    EXPECT_FALSE(panel_resized);
    EXPECT_EQ(IntProperty(grid, "cacheHintUpdateCount"), hints_before + 1);
    // Issue 754: the grid has its final width when the flag drops. The next layout passes keep
    // it and do not set the cache size again.
    const qreal grid_width_at_end = grid->width();
    ProcessEvents(100);
    EXPECT_DOUBLE_EQ(grid->width(), grid_width_at_end);
    EXPECT_EQ(IntProperty(grid, "cacheHintUpdateCount"), hints_before + 1);
    // The content follows the sidebar edge during the fold: its width changes once for each
    // step of the edge (the host sizes the loader directly) and at no other time.
    EXPECT_EQ(width_changes, edge_steps);
    EXPECT_DOUBLE_EQ(library_loader->width(), host->width());
    EXPECT_DOUBLE_EQ(sidebar->width(), sidebar_width);
  }
  EXPECT_DOUBLE_EQ(library_loader->width(), open_width);
  EXPECT_EQ(AppQmlWarnings(*loaded), QString());
}

// Issue 751: a manual toggle in the editor. The viewport slot follows the sidebar edge on every
// frame. The viewport item keeps its size and is scaled to the slot, so its render size
// (updatePresentationTargetSize) and its Resize render wait for the end of the fold and run
// once, at the slot size.
TEST_F(EditorWorkspaceSwitchQmlTest, EditorFollowsManualSidebarToggleAndRendersOnce) {
  ASSERT_TRUE(QCoreApplication::instance());
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 2);
  ASSERT_TRUE(populated.has_value());
  auto loaded = LoadMainWindowWithPackedProject(populated->packed_path_);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ProcessEvents(200);
  auto* router = loaded->host.workspace_router();
  router->OpenEditor(static_cast<uint>(populated->images_[0].file_id_),
                     static_cast<uint>(populated->images_[0].image_id_));
  ProcessEvents(800);
  ASSERT_EQ(router->workspace(), QStringLiteral("editor"));
  const MotionOn motion;

  auto* window        = loaded->window;
  auto* editor_loader = window->findChild<QQuickItem*>(QStringLiteral("editorWorkspaceLoader"));
  auto* viewport_slot = window->findChild<QQuickItem*>(QStringLiteral("editorViewportSlot"));
  auto* viewport      = window->findChild<QQuickItem*>(QStringLiteral("editorViewportItem"));
  auto* interaction =
      window->findChild<editor_rhi::EditorInteractionController*>(
          QStringLiteral("editorInteractionController"));
  ASSERT_NE(editor_loader, nullptr);
  ASSERT_NE(viewport_slot, nullptr);
  ASSERT_NE(viewport, nullptr);
  ASSERT_NE(interaction, nullptr);

  for (const bool expanded_after : {true, false}) {
    SCOPED_TRACE(expanded_after ? "open" : "fold");
    int        viewport_width_changes = 0;
    int        resize_reports         = 0;
    const auto width_connection       = QObject::connect(viewport_slot, &QQuickItem::widthChanged,
                                                         [&]() { ++viewport_width_changes; });
    const auto resize_connection      = QObject::connect(
        interaction, &editor_rhi::EditorInteractionController::viewChangeReported,
        [&](int kind) {
          if (kind ==
              static_cast<int>(editor_rhi::EditorInteractionController::ViewChangeKind::Resize)) {
            ++resize_reports;
          }
        });
    const int   sizes_before   = IntProperty(viewport_slot, "presentationSizeUpdateCount");
    const qreal viewport_width = viewport->width();
    bool        size_in_fold   = false;
    bool        resize_in_fold = false;
    bool        item_resized   = false;
    const int   edge_steps     = RunManualSidebarToggle(window, [&]() {
      size_in_fold =
          size_in_fold || IntProperty(viewport_slot, "presentationSizeUpdateCount") != sizes_before;
      resize_in_fold = resize_in_fold || resize_reports > 0;
      item_resized   = item_resized || viewport->width() != viewport_width;
    });
    QObject::disconnect(width_connection);
    QObject::disconnect(resize_connection);
    EXPECT_EQ(window->property("collectionsSidebarExpanded").toBool(), expanded_after);
    EXPECT_FALSE(size_in_fold);
    EXPECT_FALSE(resize_in_fold);
    EXPECT_FALSE(item_resized);
    EXPECT_EQ(IntProperty(viewport_slot, "presentationSizeUpdateCount"), sizes_before + 1);
    EXPECT_EQ(resize_reports, 1);
    // The slot follows the edge at most once per step: its layouts can apply two steps in one
    // polish.
    EXPECT_GE(viewport_width_changes, 1);
    EXPECT_LE(viewport_width_changes, edge_steps);
    // After the fold the viewport and the view transform have the slot size, unscaled.
    EXPECT_DOUBLE_EQ(viewport->width(), viewport_slot->width());
    EXPECT_DOUBLE_EQ(viewport->scale(), 1.0);
    EXPECT_NEAR(interaction->viewportWidth(), viewport_slot->width(), 1.0);
  }
  EXPECT_EQ(AppQmlWarnings(*loaded), QString());
}


}  // namespace
}  // namespace alcedo::ui::test
