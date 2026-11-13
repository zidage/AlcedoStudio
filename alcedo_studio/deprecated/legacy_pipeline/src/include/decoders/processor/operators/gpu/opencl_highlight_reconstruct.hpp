//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <libraw/libraw.h>

#include "image/opencl_image.hpp"

namespace alcedo {
namespace OpenCL {

void HighlightReconstruct(opencl::OpenClImage& img, LibRaw& raw_processor);

}  // namespace OpenCL
}  // namespace alcedo

#endif
