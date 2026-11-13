//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace CUDA {

// CFA-preserving 2x downsample for raw mosaic images (CV_16UC1 / CV_32FC1).
void DownsampleRaw2x(cv::cuda::GpuMat& image, RawCfaPattern& pattern,
                     cv::cuda::Stream* stream = nullptr);

// Repeated 2x downsample passes.
void DownsampleRaw(cv::cuda::GpuMat& image, RawCfaPattern& pattern, int passes,
                   cv::cuda::Stream* stream = nullptr);

}  // namespace CUDA
}  // namespace alcedo
