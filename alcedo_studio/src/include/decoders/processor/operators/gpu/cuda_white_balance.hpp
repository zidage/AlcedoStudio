//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "decoders/processor/raw_linearization_params.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace CUDA {
void ToLinearRef(cv::cuda::GpuMat& img, LibRaw& raw_processor, const RawCfaPattern& pattern,
                 cv::cuda::Stream* stream = nullptr);

/**
 * @brief Linearize U16 CFA into a preallocated F32 plane. Does not allocate @p dst.
 * @pre src is CV_16UC1, dst is CV_32FC1 with the same size, and dst already owns its storage.
 */
void ToLinearRef(const cv::cuda::GpuMat& src_u16, cv::cuda::GpuMat& dst_f32,
                 const RawLinearizationParams& params, const RawCfaPattern& pattern,
                 cv::cuda::Stream* stream = nullptr);
};
};  // namespace alcedo
