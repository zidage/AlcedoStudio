//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/lens/lens_calib_runtime.hpp"
#include "image/opencl_image.hpp"

namespace alcedo::OpenCL::Geometry {

void ApplyLensCalibration(opencl::OpenClImage& image, const LensCalibGpuParams& params);

}  // namespace alcedo::OpenCL::Geometry

#endif
