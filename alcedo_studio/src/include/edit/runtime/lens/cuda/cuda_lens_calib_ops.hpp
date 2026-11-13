//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>

#include "edit/runtime/lens/lens_calib_runtime.hpp"

namespace alcedo {
namespace CUDA {

void ApplyLensCalibration(cv::cuda::GpuMat& image, const LensCalibGpuParams& params,
                          cv::cuda::Stream* stream = nullptr);

}  // namespace CUDA
}  // namespace alcedo

