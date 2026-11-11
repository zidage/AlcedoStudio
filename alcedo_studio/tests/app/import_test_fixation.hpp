//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <exiv2/exiv2.hpp>
#include <filesystem>

#include "utils/clock/time_provider.hpp"
#include "utils/profiler/profiler.hpp"
#include "support/storage_invariants.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
class ImportServiceTests : public ::testing::Test {
 protected:
  /// The test closed its project after production writes; TearDown checks the stored rows
  /// (#805). A test that writes broken rows on purpose sets it to false.
  bool check_storage_invariants_ = true;
  test::TestTempDirectory temp_dir_;
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  // Run before any unit test runs
  void                  SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_ENABLE;
#endif
  }

  // Run before any unit test runs
  void TearDown() override {
    if (check_storage_invariants_) {
      test::ExpectStoredStorageInvariants(db_path_);
    }
#ifdef EASY_PROFILER_ENABLE
    profiler::dumpBlocksToFile(TEST_PROFILER_OUTPUT_PATH);
    EASY_PROFILER_DISABLE;
#endif
  }
};
}  // namespace alcedo