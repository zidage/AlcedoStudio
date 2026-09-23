//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/utils/color_utils.hpp"

namespace alcedo::odt_cpu {

auto ResolveACESODTRuntime(ColorUtils::ColorSpace limiting_space,
                           float peak_luminance) -> ColorUtils::ODTParams;

}  // namespace alcedo::odt_cpu
