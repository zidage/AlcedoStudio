//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <exiv2/exiv2.hpp>
#include <filesystem>

#include "utils/clock/time_provider.hpp"
#include "utils/profiler/profiler.hpp"

namespace alcedo {
class ImportServiceTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  // Run before any unit test runs
  void                  SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    // Create a unique db file location
    db_path_ = std::filesystem::temp_directory_path() / "test_db.db";
    meta_path_ = std::filesystem::temp_directory_path() / "import_service_test.json";
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_ENABLE;
#endif
  }

  // Run before any unit test runs
  void TearDown() override {
    // Clean up the DB file
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
#ifdef EASY_PROFILER_ENABLE
    profiler::dumpBlocksToFile(TEST_PROFILER_OUTPUT_PATH);
    EASY_PROFILER_DISABLE;
#endif
  }
};
}  // namespace alcedo