//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/runtime/lens/cuda/cuda_geometry_ops.hpp"

namespace alcedo {
namespace {

auto EnsureCudaDevice() -> bool {
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
}

// The Geometry panel source frame is rendered by the GPU DAG with
// DocumentGeometryUse::UncroppedSource (GpuDagCudaDrtProductTest and
// GpuDagGeometryTest). The case below covers the CUDA geometry helper only.

TEST(CudaGeometryOpsOverlayCases, ResizeLinearHandlesRoiAndEmptyWithoutAbort) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  const cv::Mat host = cv::Mat::zeros(255, 511, CV_32FC3);
  cv::cuda::GpuMat src(host);
  cv::cuda::GpuMat roi = src(cv::Rect(3, 5, 127, 63));
  cv::cuda::GpuMat dst;
  EXPECT_NO_THROW(CUDA::ResizeLinear(roi, dst, cv::Size(41, 19)));
  EXPECT_EQ(dst.cols, 41);
  EXPECT_EQ(dst.rows, 19);

  cv::cuda::GpuMat empty;
  cv::cuda::GpuMat empty_dst;
  EXPECT_NO_THROW(CUDA::ResizeLinear(empty, empty_dst, cv::Size(16, 16)));
  EXPECT_TRUE(empty_dst.empty());
}

}  // namespace
}  // namespace alcedo
