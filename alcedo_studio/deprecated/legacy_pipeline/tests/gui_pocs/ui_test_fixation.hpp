//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <exiv2/exiv2.hpp>
#include <filesystem>

#include "edit/operators/operator_registeration.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/profiler/profiler.hpp"

namespace alcedo {
class UIHistoryTests {
 protected:
  std::filesystem::path db_path_;

 public:
  // Run before any unit test runs
  void SetUp() {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    // Create a unique db file location
    db_path_ = std::filesystem::temp_directory_path() / "test_db.db";
    // Make sure there is not existing db
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    RegisterAllOperators();
#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_ENABLE;
#endif
  }

  // Run before any unit test runs
  void TearDown() {
    // Clean up the DB file
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
#ifdef EASY_PROFILER_ENABLE
    profiler::dumpBlocksToFile(TEST_PROFILER_OUTPUT_PATH);
    EASY_PROFILER_DISABLE;
#endif
  }

  auto GetDBPath() const -> std::filesystem::path { return db_path_; }
};
}  // namespace alcedo