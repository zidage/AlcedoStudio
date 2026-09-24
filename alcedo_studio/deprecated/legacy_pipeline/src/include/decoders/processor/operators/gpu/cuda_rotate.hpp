//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>

namespace alcedo {
namespace CUDA {

// Rotate the image by 90 degrees on the GPU.
// These are intentionally limited to the RAW pipeline's float images (CV_32FC3/CV_32FC4),
// because OpenCV's cv::cuda::transpose does not support elemSize() == 12 or 16.
void Rotate180(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);
void Rotate90CW(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);
void Rotate90CCW(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);

}  // namespace CUDA
}  // namespace alcedo
