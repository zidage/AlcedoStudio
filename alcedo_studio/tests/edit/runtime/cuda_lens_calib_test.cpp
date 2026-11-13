//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/runtime/lens/cuda/cuda_lens_calib_ops.hpp"
#include "lens_warp_test_support.hpp"

namespace alcedo {
namespace {

class CudaLensCalibFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    cv::cuda::setDevice(0);
  }
};

auto RunCudaLensPass(const cv::Mat& src, const LensCalibGpuParams& params) -> cv::Mat {
  cv::cuda::GpuMat image(src);
  CUDA::ApplyLensCalibration(image, params);
  cv::Mat out;
  image.download(out);
  return out;
}

}  // namespace

TEST_F(CudaLensCalibFixture, Issue713GratingKeepsAmplitudeAndMatchesCpuReference) {
  lens_warp_test::VerifyIssue713GratingAmplitude(RunCudaLensPass);
}

TEST_F(CudaLensCalibFixture, HighlightEdgeHasNoRingingAndMatchesCpuReference) {
  lens_warp_test::VerifyHighlightEdgeHasNoRinging(RunCudaLensPass);
}

TEST_F(CudaLensCalibFixture, VignettingOnlyDoesNotResample) {
  lens_warp_test::VerifyVignettingOnlyDoesNotResample(RunCudaLensPass);
}

}  // namespace alcedo
