//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>

#include "edit/operators/geometry/resize_algorithm.hpp"
#include "image/metal_image.hpp"
#include "metal/metal_utils/geometry_utils.hpp"

namespace alcedo::metal {
namespace {

auto MetalAvailable() -> bool {
  auto* device = MTL::CreateSystemDefaultDevice();
  if (device == nullptr) {
    return false;
  }
  device->release();
  return true;
}

auto MakeRamp(int type) -> cv::Mat {
  cv::Mat image(5, 7, type);
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      const float value = static_cast<float>(y * image.cols + x) / 37.0f;
      if (type == CV_32FC1) {
        image.at<float>(y, x) = value;
      } else {
        image.at<cv::Vec4f>(y, x) = {value, value * 0.5f, 1.0f - value, 1.0f};
      }
    }
  }
  return image;
}

void ExpectNear(const cv::Mat& actual, const cv::Mat& expected, float tolerance) {
  ASSERT_EQ(actual.size(), expected.size());
  ASSERT_EQ(actual.type(), expected.type());
  cv::Mat difference;
  cv::absdiff(actual, expected, difference);
  double max_difference = 0.0;
  cv::minMaxLoc(difference.reshape(1), nullptr, &max_difference);
  EXPECT_LE(max_difference, tolerance);
}

void CheckResize(int type, ResizeDownsampleAlgorithm algorithm, int interpolation) {
  const cv::Mat source_cpu = MakeRamp(type);
  cv::Mat       expected;
  cv::resize(source_cpu, expected, cv::Size(4, 3), 0.0, 0.0, interpolation);

  MetalImage source;
  source.Upload(source_cpu);
  MetalImage resized;
  utils::ResizeTexture(source, resized, cv::Size(4, 3), algorithm);

  cv::Mat actual;
  resized.Download(actual);
  ExpectNear(actual, expected, 2.0e-5f);
}

}  // namespace

TEST(MetalGeometryUtilsTest, DirectTextureBilinearMatchesOpenCvForR32AndRgba32) {
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  CheckResize(CV_32FC1, ResizeDownsampleAlgorithm::Bilinear, cv::INTER_LINEAR);
  CheckResize(CV_32FC4, ResizeDownsampleAlgorithm::Bilinear, cv::INTER_LINEAR);
}

TEST(MetalGeometryUtilsTest, DirectTextureAreaMatchesOpenCvForR32AndRgba32) {
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  CheckResize(CV_32FC1, ResizeDownsampleAlgorithm::Area, cv::INTER_AREA);
  CheckResize(CV_32FC4, ResizeDownsampleAlgorithm::Area, cv::INTER_AREA);
}

}  // namespace alcedo::metal

#endif
