//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lens/metal/metal_lens_calib.hpp"

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include "edit/runtime/metal/metal_backend.hpp"
#include "image/metal_image.hpp"
#include "lens_warp_test_support.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalLensCalibFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
  }
};

auto RunMetalLensPass(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  metal::MetalImage image;
  image.Upload(src);
  metal::ApplyLensCalibration(image, params);
  cv::Mat out;
  image.Download(out);
  return out;
}

}  // namespace

TEST_F(MetalLensCalibFixture, Issue713GratingKeepsAmplitudeAndMatchesCpuReference) {
  lens_warp_test::VerifyIssue713GratingAmplitude(RunMetalLensPass);
}

TEST_F(MetalLensCalibFixture, HighlightEdgeHasNoRingingAndMatchesCpuReference) {
  lens_warp_test::VerifyHighlightEdgeHasNoRinging(RunMetalLensPass);
}

TEST_F(MetalLensCalibFixture, VignettingOnlyDoesNotResample) {
  lens_warp_test::VerifyVignettingOnlyDoesNotResample(RunMetalLensPass);
}

}  // namespace alcedo
