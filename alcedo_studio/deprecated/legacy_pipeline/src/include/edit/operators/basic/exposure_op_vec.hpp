//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <hwy/base.h>

namespace alcedo {
HWY_INLINE void CallPixelOffset(const float* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                size_t length, float offset);
}