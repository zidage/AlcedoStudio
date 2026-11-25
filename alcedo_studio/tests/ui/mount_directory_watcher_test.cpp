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
#include <chrono>
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

// #1020: the watcher of macOS and Linux reads the mount table for the volume events.
TEST(MountDirectoryWatcherTest, QtWatcherReportsTheVolumeEventsOfTheMountTable) {
  test::TestTempDirectory temp_dir;
  const auto              card = temp_dir / "Card";
  const auto              disk = temp_dir / "Disk";
  stdfs::create_directories(card / "DCIM");
  stdfs::create_directories(disk / "Photos");
  std::vector<stdfs::path> table{temp_dir.Path(), card, disk};

  auto                     watcher = CreateQtMountDirectoryWatcher(
      kMountWatcherDirectoryLimit, [&table] { return table; }, std::chrono::milliseconds(5));
  std::vector<mount_id_t>  removed;
  std::vector<stdfs::path> removed_mount_points;
  int                      arrivals = 0;
  watcher->SetChangeCallback([](mount_id_t) {});
  watcher->SetVolumeCallbacks(
      [&removed](mount_id_t mount_id) { removed.push_back(mount_id); },
      [&arrivals] { ++arrivals; }, {},
      [&removed_mount_points](const stdfs::path& mount_point) {
        removed_mount_points.push_back(mount_point);
      });
  ASSERT_TRUE(watcher->Watch(1, card / "DCIM", {}));
  ASSERT_TRUE(watcher->Watch(2, disk / "Photos", {}));

  // The card is unmounted: only the mount on the card loses its watch, and the mount point of
  // the card is reported as gone (#1087).
  table = {temp_dir.Path(), disk};
  ASSERT_TRUE(WaitUntil([&removed] { return !removed.empty(); }));
  QThread::msleep(50);
  QCoreApplication::processEvents();
  EXPECT_EQ(removed, (std::vector<mount_id_t>{1}));
  EXPECT_EQ(removed_mount_points, (std::vector<stdfs::path>{card}));
  EXPECT_EQ(arrivals, 0);

  // The card is mounted again.
  table = {temp_dir.Path(), disk, card};
  ASSERT_TRUE(WaitUntil([&arrivals] { return arrivals > 0; }));
  QThread::msleep(50);
  QCoreApplication::processEvents();
  EXPECT_EQ(arrivals, 1);
  EXPECT_EQ(removed, (std::vector<mount_id_t>{1}));
}

// #1076: each new mount point of the mount table is reported once, with the name of its volume
// and whether its root holds a DCIM directory. A mount point of the first read is not new.
TEST(MountDirectoryWatcherTest, NewMountPointIsReportedWithItsNameAndTheDcimFlag) {
  test::TestTempDirectory temp_dir;
  const auto              card = temp_dir / "EOS_DIGITAL";
  const auto              disk = temp_dir / "Archive";
  stdfs::create_directories(card / "DCIM" / "100CANON");
  stdfs::create_directories(disk / "Photos");
  std::vector<stdfs::path> table{temp_dir.Path()};

  // The test reads the name from the directory name and the DCIM flag from the directory, as
  // ReadArrivedVolume reads them from a real volume.
  const auto               read_volume = [](const stdfs::path& mount_point) {
    ArrivedVolume volume;
    volume.mount_point        = mount_point;
    volume.name               = mount_point.filename().string();
    volume.identity           = "identity of " + volume.name;
    volume.has_dcim_directory = stdfs::is_directory(mount_point / "DCIM");
    return volume;
  };
  auto watcher = CreateQtMountDirectoryWatcher(
      kMountWatcherDirectoryLimit, [&table] { return table; }, std::chrono::milliseconds(5),
      read_volume);
  std::vector<ArrivedVolume> arrived;
  int                        arrivals = 0;
  watcher->SetChangeCallback([](mount_id_t) {});
  watcher->SetVolumeCallbacks(
      [](mount_id_t) {}, [&arrivals] { ++arrivals; },
      [&arrived](const ArrivedVolume& volume) { arrived.push_back(volume); }, {});

  table = {temp_dir.Path(), card, disk};
  ASSERT_TRUE(WaitUntil([&arrived] { return arrived.size() >= 2; }));
  QThread::msleep(50);
  QCoreApplication::processEvents();
  ASSERT_EQ(arrived.size(), 2u);
  EXPECT_EQ(arrived[0], (ArrivedVolume{card, "EOS_DIGITAL", "identity of EOS_DIGITAL", true}));
  EXPECT_EQ(arrived[1], (ArrivedVolume{disk, "Archive", "identity of Archive", false}));
  // The two mount points of one read are one arrival for the scans of the offline mounts.
  EXPECT_EQ(arrivals, 1);
}

// #1076: ReadArrivedVolume reads the DCIM flag of the root of the volume.
TEST(MountDirectoryWatcherTest, ArrivedVolumeHasTheDcimFlagOfItsRoot) {
  test::TestTempDirectory temp_dir;
  stdfs::create_directories(temp_dir / "Card" / "DCIM");
  stdfs::create_directories(temp_dir / "Disk" / "Photos" / "DCIM");
  const auto card = ReadArrivedVolume(temp_dir / "Card");
  EXPECT_EQ(card.mount_point, temp_dir / "Card");
  EXPECT_TRUE(card.has_dcim_directory);
  // A DCIM directory below the root does not count.
  EXPECT_FALSE(ReadArrivedVolume(temp_dir / "Disk").has_dcim_directory);
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
