//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <libraw/libraw.h>

#include "decoders/processor/raw_processor_pattern.hpp"
#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "image/metal_image.hpp"

namespace alcedo {
namespace metal {
void LinearizeRgb(metal::MetalImage& img, const RawRgbLinearizationParams& params);
void ToLinearRef(metal::MetalImage& img, LibRaw& raw_processor, const RawCfaPattern& pattern);
};
};
