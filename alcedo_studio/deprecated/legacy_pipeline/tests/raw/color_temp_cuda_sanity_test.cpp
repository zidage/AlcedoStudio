//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "edit/operators/basic/color_temp_op.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {
auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size <= 0) {
    return {};
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return {};
  }
  return buffer;
}
}  // namespace

TEST(ColorTempCudaSanity, RawContextAndFallbackMatrixPath) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  const auto raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "_DSC0726.ARW";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Sample RAW file is missing: " << raw_path.string();
  }

  std::vector<uint8_t> raw_bytes = ReadFileToBuffer(raw_path);
  ASSERT_FALSE(raw_bytes.empty());

  auto input = std::make_shared<ImageBuffer>(std::move(raw_bytes));

  nlohmann::json decode_params;
  decode_params["raw"] = {{"cuda", true},
                          {"highlights_reconstruct", false},
                          {"use_camera_wb", true},
                          {"backend", "alcedo"},
                          {"decode_res", 1}};

  RawDecodeOp raw_decode_op(decode_params);
  EXPECT_NO_THROW(raw_decode_op.ApplyGPU(input));

  OperatorParams params;
  raw_decode_op.SetGlobalParams(params);
  EXPECT_TRUE(params.raw_runtime_valid_);
  EXPECT_EQ(params.raw_decode_input_space_, RawDecodeInputSpace::CAMERA);

  nlohmann::json color_temp_json;
  color_temp_json["color_temp"] = {{"mode", "as_shot"}, {"cct", 6500.0f}, {"tint", 0.0f}};
  ColorTempOp color_temp_op(color_temp_json);

  // SetGlobalParams now eagerly resolves matrices internally.
  color_temp_op.SetGlobalParams(params);
  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_TRUE(std::isfinite(params.color_temp_resolved_cct_));
  EXPECT_TRUE(std::isfinite(params.color_temp_resolved_tint_));

  params.raw_camera_make_         = "UnknownMake";
  params.raw_camera_model_        = "UnknownModel";
  params.color_temp_runtime_dirty_ = true;
  // Re-resolve via SetGlobalParams with unknown camera metadata.
  color_temp_op.SetGlobalParams(params);
  EXPECT_TRUE(params.color_temp_matrices_valid_);
#endif
}
}  // namespace alcedo
