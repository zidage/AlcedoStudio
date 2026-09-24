//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/opencl_image.hpp"

namespace alcedo {
namespace OpenCL {

void Bayer2x2ToRGB_RCD(opencl::OpenClImage& image, const BayerPattern2x2& pattern);

}  // namespace OpenCL
}  // namespace alcedo

#endif
