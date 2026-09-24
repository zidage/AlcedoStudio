//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include "decoders/processor/operators/gpu/opencl_xtrans_interpolate.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

auto MakeXTransPattern() -> XTransPattern6x6 {
  static constexpr int kRawFc[36] = {
      1, 2, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 1,
      1, 2, 1, 1, 0, 1, 1, 0, 1, 2, 1, 2, 2, 1, 2, 0, 1, 0,
  };

  XTransPattern6x6 pattern = {};
  for (int i = 0; i < 36; ++i) {
    pattern.raw_fc[i] = kRawFc[i];
    pattern.rgb_fc[i] = FoldRawColorToRgb(kRawFc[i]);
  }
  return pattern;
}

auto MakeXTransRaw(int rows, int cols, const XTransPattern6x6& pattern) -> cv::Mat {
  cv::Mat raw(rows, cols, CV_32FC1);
  for (int y = 0; y < rows; ++y) {
    float* row = raw.ptr<float>(y);
    for (int x = 0; x < cols; ++x) {
      static constexpr float kByColor[3] = {0.75f, 0.52f, 0.21f};
      row[x] = kByColor[RgbColorAt(pattern, y, x)] + 0.001f * float((7 * y + 3 * x) % 11);
    }
  }
  return raw;
}

}  // namespace

TEST(OpenClRawOpsTest, XTransInterpolateProducesRGBAAndPreservesKnownSamples) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  auto& context = OpenClContext::Instance();
  if (!context.TryInitialize()) {
    GTEST_SKIP() << context.LastInitializationError();
  }
  PrepareOpenClRuntime();

  const XTransPattern6x6 pattern = MakeXTransPattern();
  const cv::Mat          raw     = MakeXTransRaw(18, 18, pattern);

  opencl::OpenClImage image;
  image.Upload(raw);
  ASSERT_NO_THROW(OpenCL::XTransToRGB_Ref(image, pattern, 1));

  cv::Mat rgba;
  image.Download(rgba);

  ASSERT_EQ(rgba.type(), CV_32FC4);
  ASSERT_EQ(rgba.size(), raw.size());
  EXPECT_TRUE(cv::checkRange(rgba, true, nullptr, 0.0, 4.0));

  for (int y = 0; y < rgba.rows; ++y) {
    const float*     raw_row  = raw.ptr<float>(y);
    const cv::Vec4f* rgba_row = rgba.ptr<cv::Vec4f>(y);
    for (int x = 0; x < rgba.cols; ++x) {
      const int color = RgbColorAt(pattern, y, x);
      EXPECT_NEAR(rgba_row[x][3], 1.0f, 1e-6);
      EXPECT_NEAR(rgba_row[x][color], raw_row[x], 1e-6);
    }
  }
#endif
}

}  // namespace alcedo
