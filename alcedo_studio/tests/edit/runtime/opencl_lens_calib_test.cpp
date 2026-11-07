//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <opencv2/core.hpp>

#include "edit/runtime/lens/opencl/opencl_lens_calib_ops.hpp"
#include "image/opencl_image.hpp"
#include "lens_warp_test_support.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

class OpenClLensCalibFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
  }
};

auto RunOpenClLensPass(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  opencl::OpenClImage image;
  image.Upload(src);
  OpenCL::Geometry::ApplyLensCalibration(image, params);
  cv::Mat out;
  image.Download(out);
  return out;
}

}  // namespace

TEST_F(OpenClLensCalibFixture, Issue713GratingKeepsAmplitudeAndMatchesCpuReference) {
  lens_warp_test::VerifyIssue713GratingAmplitude(RunOpenClLensPass);
}

TEST_F(OpenClLensCalibFixture, HighlightEdgeHasNoRingingAndMatchesCpuReference) {
  lens_warp_test::VerifyHighlightEdgeHasNoRinging(RunOpenClLensPass);
}

TEST_F(OpenClLensCalibFixture, VignettingOnlyDoesNotResample) {
  lens_warp_test::VerifyVignettingOnlyDoesNotResample(RunOpenClLensPass);
}

}  // namespace alcedo
