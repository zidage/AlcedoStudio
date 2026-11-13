//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/// What a DisplayToAp1 dispatch writes.
enum class DisplayToAp1Output : std::uint8_t {
  /// ACEScc-encoded AP1: the develop_output encoding that the Color Grades read.
  AcesccAp1,
  /// ACES2065-1 (AP0) linear after the display-referred inverse, before the AP1 clamp. Used to
  /// check the inverse against the OpenColorIO reference.
  LinearAp0,
};

}  // namespace alcedo
