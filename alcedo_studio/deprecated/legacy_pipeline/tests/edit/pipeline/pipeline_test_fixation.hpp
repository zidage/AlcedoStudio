//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <exiv2/exiv2.hpp>
#include <filesystem>

#include "edit/operators/operator_registeration.hpp"
#include "utils/clock/time_provider.hpp"
#include <opencv2/core/utils/logger.hpp>


namespace alcedo {
class PipelineTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;

  // Run before any unit test runs
  void                  SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
    // Create a unique db file location
    db_path_ = std::filesystem::temp_directory_path() / "test_db.db";
    // Make sure there is not existing db
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    RegisterAllOperators();
  }

  // Run before any unit test runs
  void TearDown() override {
    // Clean up the DB file
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
  }
};
}  // namespace alcedo