//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF7 (#989), P11: the directory watchers of the mounted folders on real
// temporary directories.

#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "support/test_temp_directory.hpp"

namespace alcedo::ui {
namespace {

namespace stdfs = std::filesystem;

/// Process events until @p done is true or 10 s pass. Returns @p done().
auto WaitUntil(const std::function<bool()>& done) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.elapsed() > 10000) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return true;
}

void WriteText(const stdfs::path& path) { std::ofstream(path) << "x"; }

TEST(MountDirectoryWatcherTest, PlatformWatcherReportsANewFileInASubdirectory) {
  test::TestTempDirectory temp_dir;
  const auto              root = temp_dir / "Japan";
  stdfs::create_directories(root / "Day 1" / "Morning");

  auto watcher = CreatePlatformMountDirectoryWatcher();
  int  changes = 0;
  watcher->SetChangeCallback([&changes](mount_id_t mount_id) {
    EXPECT_EQ(mount_id, 7U);
    ++changes;
  });
  const std::vector<std::wstring> directories{L"Day 1", L"Day 1/Morning"};
  ASSERT_TRUE(watcher->Watch(7, root, directories));

  WriteText(root / "Day 1" / "Morning" / "c.jpg");
  EXPECT_TRUE(WaitUntil([&changes] { return changes > 0; }));

  // No notification after the mount lost its watch.
  watcher->Unwatch(7);
  QCoreApplication::processEvents();
  changes = 0;
  WriteText(root / "Day 1" / "d.jpg");
  QThread::msleep(200);
  QCoreApplication::processEvents();
  EXPECT_EQ(changes, 0);
}

TEST(MountDirectoryWatcherTest, QtWatcherRefusesAMountThatPassesTheDirectoryLimit) {
  test::TestTempDirectory temp_dir;
  stdfs::create_directories(temp_dir / "A" / "One");
  stdfs::create_directories(temp_dir / "B" / "One");
  stdfs::create_directories(temp_dir / "B" / "Two");

  // The root counts as one directory: A needs 2, B needs 3.
  auto watcher = CreateQtMountDirectoryWatcher(4);
  watcher->SetChangeCallback([](mount_id_t) {});
  ASSERT_TRUE(watcher->Watch(1, temp_dir / "A", std::vector<std::wstring>{L"One"}));
  EXPECT_FALSE(watcher->Watch(2, temp_dir / "B", std::vector<std::wstring>{L"One", L"Two"}));

  // The directories of a removed watch are free again.
  watcher->Unwatch(1);
  EXPECT_TRUE(watcher->Watch(2, temp_dir / "B", std::vector<std::wstring>{L"One", L"Two"}));
}

TEST(MountDirectoryWatcherTest, WatchOfAMissingDirectoryFails) {
  test::TestTempDirectory                                                    temp_dir;
  const std::vector<std::function<std::unique_ptr<MountDirectoryWatcher>()>> factories{
      [] { return CreatePlatformMountDirectoryWatcher(); },
      [] { return CreateQtMountDirectoryWatcher(); }};
  for (const auto& create : factories) {
    auto watcher = create();
    watcher->SetChangeCallback([](mount_id_t) {});
    EXPECT_FALSE(watcher->Watch(1, temp_dir / "Unplugged", {}));
  }
}

#ifdef _WIN32
// A change notification handle in a directory prevents a rename of each directory above it.
// The Windows watcher holds one handle on the root, so the user can still rename a mounted
// directory that has subdirectories.
TEST(MountDirectoryWatcherTest, WindowsWatcherLeavesMountedDirectoriesRenamable) {
  test::TestTempDirectory temp_dir;
  const auto              root = temp_dir / "Japan";
  stdfs::create_directories(root / "Day 1" / "Morning");

  auto watcher = CreatePlatformMountDirectoryWatcher();
  int  changes = 0;
  watcher->SetChangeCallback([&changes](mount_id_t) { ++changes; });
  const std::vector<std::wstring> directories{L"Day 1", L"Day 1/Morning"};
  ASSERT_TRUE(watcher->Watch(1, root, directories));

  std::error_code ec;
  stdfs::rename(root / "Day 1", root / "Day One", ec);
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_TRUE(WaitUntil([&changes] { return changes > 0; }));
  watcher->UnwatchAll();
}
#endif

}  // namespace
}  // namespace alcedo::ui
