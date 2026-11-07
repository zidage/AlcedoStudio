//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/image_analysis_encoder.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "app/thumbnail_service.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

// Builds a ThumbnailGuard whose ImageBuffer holds a small CPU mat of the given type,
// simulating a host-rendered k1024 thumbnail that is already CPU-resident.
auto MakeGuardWithMat(int width, int height, int type) -> std::shared_ptr<ThumbnailGuard> {
  cv::Mat mat(height, width, type, cv::Scalar(100, 150, 200, 255));
  auto    guard = std::make_shared<ThumbnailGuard>();
  guard->thumbnail_buffer_ = std::make_unique<ImageBuffer>(std::move(mat));
  return guard;
}

auto CountRegularFiles(const std::filesystem::path& dir) -> size_t {
  if (!std::filesystem::exists(dir)) {
    return 0;
  }
  size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      ++n;
    }
  }
  return n;
}

auto ScratchDir(const std::string& tag) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("alcedo_ia_enc_test_" + tag);
}

TEST(ImageAnalysisEncoderTest, EncodesThumbnailAsJpegByDefault) {
  auto       guard    = MakeGuardWithMat(64, 48, CV_8UC3);
  const auto temp_dir = ScratchDir("jpeg");
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  std::string       err;
  const EncodedRendition encoded =
      EncodeThumbnailForRemoteAnalysis(*guard, 90, 1024, temp_dir, &err);
  EXPECT_TRUE(encoded.ok) << err;
  EXPECT_EQ(encoded.mime_type, "image/jpeg");
  EXPECT_EQ(encoded.format_hint, "image/jpeg;max_edge=64");
  EXPECT_EQ(encoded.rendition_kind, "thumbnail");
  EXPECT_EQ(encoded.width, 64u);
  EXPECT_EQ(encoded.height, 48u);
  EXPECT_EQ(encoded.max_edge, 64u);
  ASSERT_GE(encoded.bytes.size(), 2u);
  EXPECT_EQ(encoded.bytes[0], 0xFF);  // JPEG SOI magic
  EXPECT_EQ(encoded.bytes[1], 0xD8);
  // The encoded hint must NOT be the raw RGBA8 embedding shape.
  EXPECT_EQ(encoded.format_hint.find("rgba8"), std::string::npos);
  // RAII cleanup: no temp file leaked after a successful encode.
  EXPECT_EQ(CountRegularFiles(temp_dir), 0u);

  std::filesystem::remove_all(temp_dir);
}

TEST(ImageAnalysisEncoderTest, EncodesFromRgbaAndFloatInputs) {
  // 4-channel RGBA and 32F inputs must both encode (channel/depth normalization to
  // CV_8UC3 RGB), proving the encoder is robust to the thumbnail pipeline's varied
  // outputs without using OpenCV imgcodecs.
  for (int type : {CV_8UC4, CV_32FC3, CV_8UC1}) {
    auto       guard    = MakeGuardWithMat(32, 32, type);
    const auto temp_dir = ScratchDir("types");
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    std::string err;
    const EncodedRendition encoded =
        EncodeThumbnailForRemoteAnalysis(*guard, 85, 1024, temp_dir, &err);
    EXPECT_TRUE(encoded.ok) << err;
    EXPECT_EQ(encoded.mime_type, "image/jpeg");
    EXPECT_EQ(encoded.max_edge, 32u);
    EXPECT_EQ(CountRegularFiles(temp_dir), 0u);

    std::filesystem::remove_all(temp_dir);
  }
}

TEST(ImageAnalysisEncoderTest, NullBufferFailsCleanlyWithoutLeakingTempFiles) {
  ThumbnailGuard guard;  // null thumbnail_buffer_
  const auto     temp_dir = ScratchDir("null");
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  std::string err;
  const EncodedRendition encoded =
      EncodeThumbnailForRemoteAnalysis(guard, 90, 1024, temp_dir, &err);
  EXPECT_FALSE(encoded.ok);
  EXPECT_FALSE(err.empty());
  // Failure path must not leak a temp file.
  EXPECT_EQ(CountRegularFiles(temp_dir), 0u);

  std::filesystem::remove_all(temp_dir);
}

}  // namespace
}  // namespace alcedo
