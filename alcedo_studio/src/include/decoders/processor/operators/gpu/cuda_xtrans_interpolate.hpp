//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace CUDA {

void XTransToRGB_Ref(cv::cuda::GpuMat& image, const XTransPattern6x6& pattern, int passes);

/**
 * @brief X-Trans interpolate into preallocated green (F32C1) and RGB (F32C3) planes.
 * @pre green and output match @p raw size; neither is allocated by this function.
 */
void XTransToRGB_Ref(const cv::cuda::GpuMat& raw, cv::cuda::GpuMat& green, cv::cuda::GpuMat& output,
                     const XTransPattern6x6& pattern, int passes, cv::cuda::Stream* stream);

}  // namespace CUDA
}  // namespace alcedo
