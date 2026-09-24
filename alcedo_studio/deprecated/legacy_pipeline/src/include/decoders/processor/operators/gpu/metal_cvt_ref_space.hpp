//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "image/metal_image.hpp"

namespace alcedo {
namespace metal {
void ApplyInverseCamMul(MetalImage& img, const float* cam_mul);
}  // namespace metal
}  // namespace alcedo

#endif
