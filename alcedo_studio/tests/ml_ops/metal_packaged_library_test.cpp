// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

class MetalPackagedLibraryTest : public ::testing::Test {
 protected:
  std::filesystem::path previous_directory;
  std::filesystem::path directory;

  void                  SetUp() override {
    previous_directory = std::filesystem::current_path();
    ASSERT_NO_THROW((void)metal::ComputePipelineCache::Instance().GetPipelineState(
        ALCEDO_TEST_METALLIB, "convert_r32f_to_r32f", "build Metal library"));
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = std::filesystem::path(ALCEDO_TEST_TEMP_ROOT) / std::to_string(suffix);
    std::filesystem::create_directories(directory / "metallib");
    std::filesystem::current_path(directory);
    ASSERT_TRUE(std::filesystem::is_regular_file(ALCEDO_TEST_METALLIB));
  }

  void TearDown() override {
    if (!previous_directory.empty()) {
      std::filesystem::current_path(previous_directory);
    }
    if (!directory.empty()) {
      std::filesystem::remove_all(directory);
    }
  }
};

TEST_F(MetalPackagedLibraryTest, MissingPackagedLibraryDoesNotLoadExistingBuildLibrary) {
  EXPECT_THROW((void)metal::ComputePipelineCache::Instance().GetPipelineState(
                   ALCEDO_TEST_METALLIB, "convert_r32f_to_r32f", "packaged Metal library"),
               std::runtime_error);
}

TEST_F(MetalPackagedLibraryTest, InvalidPackagedLibraryDoesNotLoadExistingBuildLibrary) {
  std::ofstream(directory / "metallib/metal_convert.metallib") << "invalid Metal library\n";
  EXPECT_THROW((void)metal::ComputePipelineCache::Instance().GetPipelineState(
                   ALCEDO_TEST_METALLIB, "convert_r32f_to_r32f", "packaged Metal library"),
               std::runtime_error);
}

}  // namespace
}  // namespace alcedo
