//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "image/opencl_image.hpp"

namespace alcedo {
namespace OpenCL {

void ApplyInverseCamMul(opencl::OpenClImage& img, const float* cam_mul);

}  // namespace OpenCL
}  // namespace alcedo

#endif
