//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/runtime/lens/cuda/cuda_geometry_ops.hpp"
#include "edit/operators/geometry/resize_op.hpp"
#include "image/gpu_backend.hpp"
#include "image/image_buffer.hpp"

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

auto MakeGradientGpuBuffer(int width, int height, int type) -> std::shared_ptr<ImageBuffer> {
  cv::Mat host(height, width, type);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(std::max(width - 1, 1));
      const float fy = static_cast<float>(y) / static_cast<float>(std::max(height - 1, 1));
      if (type == CV_32FC3) {
        host.at<cv::Vec3f>(y, x) = cv::Vec3f(fx, fy, 0.5f * (fx + fy));
      } else {
        host.at<cv::Vec4f>(y, x) = cv::Vec4f(fx, fy, 0.5f * (fx + fy), 1.0f);
      }
    }
  }
  auto buffer = std::make_shared<ImageBuffer>(std::move(host));
  buffer->SyncToGPU(GpuBackendKind::CUDA);
  return buffer;
}

// The Geometry panel source frame is rendered by the GPU DAG with
// DocumentGeometryUse::UncroppedSource (GpuDagCudaDrtProductTest and
// GpuDagGeometryTest). The cases below cover the ResizeOp and CUDA geometry
// helpers only.

// Adversarial ResizeOp GPU cases that ordinary unit tests skip: ROI of a shared
// GpuMat, bilinear downscale (the overlay FAST_PREVIEW algorithm), odd sizes.
TEST(ResizeOpCudaOverlayCases, BilinearRoiDownscaleOnSharedGpuMatDoesNotAbort) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  auto buffer = MakeGradientGpuBuffer(401, 307, CV_32FC4);
  auto shared = std::make_shared<ImageBuffer>();
  shared->ShareGPUDataFrom(*buffer);

  nlohmann::json params;
  params["resize"] = {{"enable_scale", true},
                      {"maximum_edge", 97},
                      {"enable_roi", true},
                      {"downsample_algorithm", "bilinear"},
                      {"roi",
                       {{"x", 17},
                        {"y", 11},
                        {"resize_factor_x", 0.63f},
                        {"resize_factor_y", 0.41f},
                        {"resize_factor", 0.63f},
                        {"reference_width", 401},
                        {"reference_height", 307}}}};

  ResizeOp op(params);
  EXPECT_NO_THROW(op.ApplyGPU(shared));
  ASSERT_TRUE(shared->gpu_data_valid_);
  EXPECT_GT(shared->GetGPUWidth(), 0);
  EXPECT_GT(shared->GetGPUHeight(), 0);
  EXPECT_LE(std::max(shared->GetGPUWidth(), shared->GetGPUHeight()), 97);
}

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
