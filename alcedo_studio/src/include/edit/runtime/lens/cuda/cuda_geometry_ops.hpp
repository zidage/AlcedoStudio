//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>

namespace alcedo {
namespace CUDA {

void Downsample2xBox(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst);

void ResizeLinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size);

void ResizeAreaApprox(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size);

void WarpAffineLinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, const cv::Mat& matrix,
                      cv::Size out_size, const cv::Scalar& border_value);

}  // namespace CUDA
}  // namespace alcedo
