//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/metal_image.hpp"

namespace alcedo {
namespace metal {

void XTransToRGB_Ref(MetalImage& image, const XTransPattern6x6& pattern, int passes);

}  // namespace metal
}  // namespace alcedo

#endif
