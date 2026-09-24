//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <libraw/libraw.h>

#include "decoders/processor/raw_processor_pattern.hpp"
#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "image/opencl_image.hpp"

namespace alcedo {
namespace OpenCL {

void LinearizeRgb(opencl::OpenClImage& img, const RawRgbLinearizationParams& params);

void ToLinearRef(opencl::OpenClImage& img, LibRaw& raw_processor, const RawCfaPattern& pattern);

}  // namespace OpenCL
}  // namespace alcedo

#endif
