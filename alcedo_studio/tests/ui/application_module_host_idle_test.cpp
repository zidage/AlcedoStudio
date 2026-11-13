//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

class ApplicationModuleHostTest : public ApplicationModuleHostTestFixture {};

auto CollectCiRawFiles() -> std::vector<std::filesystem::path> {
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

TEST_F(ApplicationModuleHostTest, IsIdleIsFalseWhileImportRuns) {
  const auto raw_files = CollectCiRawFiles();
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  ASSERT_TRUE(WaitUntil([&] { return host.IsIdle(); }, std::chrono::seconds(30)));

  host.import_export()->StartImport(PathsToQStringList(raw_files));

  EXPECT_TRUE(host.import_export()->ImportRunning());
  EXPECT_FALSE(host.IsIdle());
  ASSERT_TRUE(
      WaitUntil([&] { return !host.import_export()->ImportRunning(); }, std::chrono::minutes(2)));
  EXPECT_TRUE(WaitUntil([&] { return host.IsIdle(); }, std::chrono::seconds(30)));
  EXPECT_GE(host.import_export()->ImportCompleted(), 1);
}

}  // namespace
}  // namespace alcedo::ui::test
