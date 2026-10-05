//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Sample sets shared by the host, CUDA and OpenCL tests of color/color_encoding_math.h.

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "color/color_encoding_math.h"

namespace alcedo::color_encoding_test {

/// Number of evenly spaced code values in [0, 1] (plan L1: 4096 code values).
inline constexpr int kCodeValueCount = 4096;

/// Code values i / 4095 for i in [0, 4095].
inline auto          CodeValues() -> std::vector<float> {
  std::vector<float> codes(kCodeValueCount);
  for (int i = 0; i < kCodeValueCount; ++i) {
    codes[static_cast<std::size_t>(i)] = static_cast<float>(i) / (kCodeValueCount - 1);
  }
  return codes;
}

/// Linear values from 0 and 2^-14 to 2^6 (geometric), covering every curve's toe and shoulder.
inline auto LinearValues() -> std::vector<float> {
  std::vector<float> values(kCodeValueCount);
  values[0] = 0.0f;
  for (int i = 1; i < kCodeValueCount; ++i) {
    values[static_cast<std::size_t>(i)] =
        std::exp2(-14.0f + 20.0f * static_cast<float>(i - 1) / (kCodeValueCount - 2));
  }
  return values;
}

/// |a - b| scaled by max(1, |b|): absolute below 1, relative above.
inline auto ScaledDifference(float a, float b) -> float {
  return std::abs(a - b) / std::fmax(1.0f, std::abs(b));
}

}  // namespace alcedo::color_encoding_test
