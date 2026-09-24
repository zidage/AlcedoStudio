//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <opencv2/core.hpp>
#include <stdexcept>

#include "decoders/processor/operators/gpu/metal_encode.hpp"
#include "image/metal_image.hpp"
#include "metal/metal_context.hpp"
#include "metal/metal_utils/metal_convert_utils.hpp"

namespace alcedo {
namespace {

auto MakeRGBAImage(int rows, int cols) -> cv::Mat {
  cv::Mat rgba(rows, cols, CV_32FC4);
  for (int y = 0; y < rows; ++y) {
    cv::Vec4f* row = rgba.ptr<cv::Vec4f>(y);
    for (int x = 0; x < cols; ++x) {
      row[x] = cv::Vec4f(static_cast<float>(x) / 17.0f, static_cast<float>(y) / 13.0f,
                         static_cast<float>(x + y) / 29.0f, 1.0f);
    }
  }
  return rgba;
}

auto MakeClampedImage() -> cv::Mat {
  cv::Mat img(5, 6, CV_32FC1);
  for (int y = 0; y < img.rows; ++y) {
    float* row = img.ptr<float>(y);
    for (int x = 0; x < img.cols; ++x) {
      row[x] = -0.35f + static_cast<float>(y * img.cols + x) * 0.11f;
    }
  }
  return img;
}

void InitHighlightRawProcessor(LibRaw& raw_processor) {
  raw_processor.imgdata.color.cam_mul[0] = 2.15f;
  raw_processor.imgdata.color.cam_mul[1] = 1.0f;
  raw_processor.imgdata.color.cam_mul[2] = 1.42f;
  raw_processor.imgdata.color.cam_mul[3] = 1.0f;
}

auto MetalRuntimeAvailable() -> bool {
  try {
    return MetalContext::Instance().Device() != nullptr;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

TEST(MetalRawOpsTest, CropRectMatchesCPUReference) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalRuntimeAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  const cv::Mat     src = MakeRGBAImage(9, 11);
  const cv::Rect    crop_rect(2, 3, 5, 4);

  metal::MetalImage image;
  image.Upload(src);

  metal::MetalImage cropped;
  ASSERT_NO_THROW(image.CropTo(cropped, crop_rect));

  cv::Mat cropped_cpu;
  cropped.Download(cropped_cpu);

  const cv::Mat expected = src(crop_rect).clone();
  ASSERT_EQ(cropped_cpu.type(), CV_32FC4);
  ASSERT_EQ(cropped_cpu.size(), expected.size());
  EXPECT_LE(cv::norm(cropped_cpu, expected, cv::NORM_INF), 1e-6);
#endif
}

TEST(MetalRawOpsTest, ClampTextureOnlyClampsUpperBound) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalRuntimeAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  const cv::Mat     src = MakeClampedImage();

  metal::MetalImage image;
  image.Upload(src);

  ASSERT_NO_THROW(metal::utils::ClampTexture(image));

  cv::Mat clamped_gpu;
  image.Download(clamped_gpu);

  cv::Mat expected = src.clone();
  cv::min(expected, 1.0f, expected);

  ASSERT_EQ(clamped_gpu.type(), CV_32FC1);
  ASSERT_EQ(clamped_gpu.size(), expected.size());
  EXPECT_LE(cv::norm(clamped_gpu, expected, cv::NORM_INF), 1e-6);
#endif
}

TEST(MetalRawOpsTest, EncodedHighlightReconstructRaisesClippedChannelInPartialGroups) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalRuntimeAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);

  constexpr int  rows = 43;
  constexpr int  cols = 45;
  const cv::Rect core(12, 11, 20, 18);
  cv::Mat        input(rows, cols, CV_32FC4, cv::Scalar(1.2f, 0.95f, 1.0f, 1.0f));
  for (int y = core.y; y < core.y + core.height; ++y) {
    auto* row = input.ptr<cv::Vec4f>(y);
    for (int x = core.x; x < core.x + core.width; ++x) {
      row[x] = cv::Vec4f(2.0f, 1.0f, 1.3f, 1.0f);
    }
  }

  metal::MetalImage encoded_source;
  encoded_source.Upload(input);
  metal::MetalImage encoded_output;
  encoded_output.Create(cols, rows, metal::PixelFormat::RGBA32FLOAT, true, true, false);

  auto* device = MetalContext::Instance().Device();
  auto* queue  = MetalContext::Instance().Queue();
  ASSERT_NE(device, nullptr);
  ASSERT_NE(queue, nullptr);
  auto stats =
      NS::TransferPtr(device->newBuffer(6 * sizeof(float), MTL::ResourceStorageModeShared));
  ASSERT_TRUE(stats);
  std::memset(stats->contents(), 0, stats->length());
  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  ASSERT_TRUE(command_buffer);
  ASSERT_NO_THROW(metal::EncodeHighlightReconstruct(
      command_buffer.get(), encoded_source.Texture(), encoded_output.Texture(), stats.get(), 0,
      raw_processor.imgdata.color.cam_mul, cols, rows));
  command_buffer->commit();
  command_buffer->waitUntilCompleted();

  cv::Mat output;
  encoded_output.Download(output);
  ASSERT_EQ(output.type(), CV_32FC4);
  ASSERT_EQ(output.size(), input.size());

  const cv::Vec4f before = input.at<cv::Vec4f>(core.y + core.height / 2, core.x + core.width / 2);
  const cv::Vec4f after  = output.at<cv::Vec4f>(core.y + core.height / 2, core.x + core.width / 2);
  EXPECT_NEAR(after[0], before[0], 1e-5f);
  EXPECT_GT(after[1], before[1] + 0.1f);
  EXPECT_NEAR(after[2], before[2], 1e-5f);
  EXPECT_NEAR(after[3], before[3], 1e-6f);

  float reconstructed_green_min = std::numeric_limits<float>::max();
  float reconstructed_green_max = std::numeric_limits<float>::lowest();
  for (int y = core.y + 2; y < core.y + core.height - 2; ++y) {
    const auto* row = output.ptr<cv::Vec4f>(y);
    for (int x = core.x + 2; x < core.x + core.width - 2; ++x) {
      reconstructed_green_min = std::min(reconstructed_green_min, row[x][1]);
      reconstructed_green_max = std::max(reconstructed_green_max, row[x][1]);
    }
  }
  EXPECT_LT(reconstructed_green_max - reconstructed_green_min, 1e-4f);
#endif
}

}  // namespace alcedo
