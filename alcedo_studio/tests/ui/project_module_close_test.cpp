//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ProjectModule::CloseProject tests: a closed project persists (or discards) its changes, and a
// reopened project lists the same library items.

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

class ProjectModuleTest : public ApplicationModuleHostTestFixture {};

auto CollectCiRawFiles(size_t max_count) -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

auto ImportAndWait(ApplicationModuleHost& host, const std::vector<std::filesystem::path>& files)
    -> bool {
  host.import_export()->StartImport(PathsToQStringList(files));
  return WaitUntil([&] { return host.IsIdle(); }, std::chrono::minutes(2));
}

auto LibraryElementIds(ApplicationModuleHost& host) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& item : host.library()->view_state().all_images_) {
    ids.push_back(item.element_id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

auto ReopenProject(ApplicationModuleHost& host, const std::filesystem::path& package) -> bool {
  QSignalSpy changed(host.project(), &ProjectModule::ProjectChanged);
  if (!host.project()->LoadProject(PathToQString(package))) {
    return false;
  }
  WaitForSignal(changed, 15000);
  ProcessEvents(500);
  return host.project()->ServiceReady() && host.project()->ProjectEntered();
}

TEST_F(ProjectModuleTest, CloseProjectPersistsAndReopenShowsSameItems) {
  const auto raw_files = CollectCiRawFiles(2);
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  ASSERT_TRUE(ImportAndWait(host, raw_files));
  ProcessEvents(500);
  const auto imported_ids = LibraryElementIds(host);
  ASSERT_EQ(imported_ids.size(), raw_files.size());
  const auto package = host.project()->handler().package_path();

  QSignalSpy changed(host.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(host.project()->CloseProject(/*persist=*/true))
      << host.project()->ServiceMessage().toStdString();
  EXPECT_EQ(changed.count(), 1);
  EXPECT_FALSE(host.project()->ServiceReady());
  EXPECT_FALSE(host.project()->ProjectEntered());
  EXPECT_FALSE(host.project()->handler().project());
  EXPECT_TRUE(LibraryElementIds(host).empty());
  EXPECT_FALSE(host.project()->CloseProject(true));

  ASSERT_TRUE(ReopenProject(host, package)) << host.project()->ServiceMessage().toStdString();
  EXPECT_EQ(LibraryElementIds(host), imported_ids);
}

TEST_F(ProjectModuleTest, CloseProjectWithoutPersistWritesNoPackage) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  // A new project has no package file until its first save.
  const auto package = host.project()->handler().package_path();
  ASSERT_FALSE(package.empty());
  ASSERT_FALSE(std::filesystem::exists(package));

  ASSERT_TRUE(host.project()->CloseProject(/*persist=*/false));
  EXPECT_FALSE(host.project()->ServiceReady());
  EXPECT_FALSE(std::filesystem::exists(package));
}

TEST_F(ProjectModuleTest, CloseProjectWithPersistWritesPackage) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  const auto package = host.project()->handler().package_path();
  ASSERT_FALSE(std::filesystem::exists(package));

  ASSERT_TRUE(host.project()->CloseProject(/*persist=*/true));
  EXPECT_TRUE(std::filesystem::exists(package));
}

TEST_F(ProjectModuleTest, CloseProjectIsRejectedWhileImportRuns) {
  const auto raw_files = CollectCiRawFiles(2);
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  host.import_export()->StartImport(PathsToQStringList(raw_files));
  ASSERT_TRUE(host.import_export()->ImportRunning());

  EXPECT_FALSE(host.project()->ProjectCloseBlockReason().isEmpty());
  EXPECT_FALSE(host.project()->CloseProject(true));
  EXPECT_TRUE(host.project()->ServiceReady());
  ASSERT_TRUE(WaitUntil([&] { return host.IsIdle(); }, std::chrono::minutes(2)));
}

}  // namespace
}  // namespace alcedo::ui::test
