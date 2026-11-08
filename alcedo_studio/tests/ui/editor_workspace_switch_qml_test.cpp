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

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPoint>
#include <QQuickItem>
#include <QQuickWindow>
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
#include "ui/main_qml_test_fixture.hpp"

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

  DatabaseLockHold hold(loaded->host.project()->handler().project()->GetStorage()->GetDatabase());
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

}  // namespace
}  // namespace alcedo::ui::test
