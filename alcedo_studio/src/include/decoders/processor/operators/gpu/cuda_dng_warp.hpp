//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>

#include "decoders/dng_default_crop.hpp"

namespace alcedo {
namespace CUDA {

/**
 * @brief Apply DNG OpcodeList3 WarpRectilinear from @p src into caller-owned @p dst.
 *
 * Images must have identical extent and CV_32FC3 or CV_32FC4 type. The function enqueues work on
 * @p stream and does not allocate GPU storage. Throws for incompatible images or CUDA failure.
 */
void WarpDngRectilinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                        const dng::WarpRectilinear& warp, cv::cuda::Stream* stream = nullptr);

void ApplyDngWarpRectilinear(cv::cuda::GpuMat& img, const dng::WarpRectilinear& warp,
                             cv::cuda::Stream* stream = nullptr);

}  // namespace CUDA
}  // namespace alcedo
