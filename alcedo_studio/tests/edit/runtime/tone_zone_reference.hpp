//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>

/**
 * @file tone_zone_reference.hpp
 * @brief Host reference for the Color Grade Whites and Blacks adjustments.
 *
 * The CUDA, Metal, and OpenCL primary-grade kernels implement the same math
 * (ToneZoneShiftEv in cuda_primary_grade_pass.cu documents it); GPU tests compare their output
 * against these functions.
 *
 * Each pixel moves by one exposure shift, chosen by its ACEScc luma (stops x from 18% grey):
 *   - Whites works on x > 0, Blacks on x < 0. The zone is 3 stops wide: smoothstep weight from
 *     grey (no shift) to 3 stops from grey (the full shift s), and the full shift past it.
 *   - s = value * 0.03 stops, so +/-100 is +/-3 stops.
 *   - Whites above 0 and Blacks below 0 apply x + s * weight. The other direction is the exact
 *     inverse of that curve, so +v and -v undo each other and every curve is monotonic.
 *   - Blacks is Whites mirrored about grey: shift_b(x, s) = -shift_w(-x, -s).
 */
namespace alcedo::tone_zone_reference {

using Rgb = std::array<float, 3>;

inline constexpr float kZoneWidthEv   = 3.0f;
inline constexpr float kEvPerUnit     = 0.03f;
inline constexpr float kAcesccB       = 17.52f;
inline constexpr float kAcesccMidGrey = 0.41358840f;

/// Whites shift in stops at @p x stops from grey, for a full shift of @p s stops.
inline auto ShiftEv(float x, float s) -> float {
  if (x <= 0.0f || s == 0.0f) return 0.0f;
  const float w = kZoneWidthEv;
  if (s > 0.0f) {
    const float t = std::min(x / w, 1.0f);
    return s * t * t * (3.0f - 2.0f * t);
  }
  const float a = -s;
  if (x >= w + a) return -a;
  float t = x / (w + a);
  for (int i = 0; i < 5; ++i) {
    const float f  = w * t + a * t * t * (3.0f - 2.0f * t) - x;
    const float fp = w + 6.0f * a * t * (1.0f - t);
    t              = std::clamp(t - f / fp, 0.0f, 1.0f);
  }
  return w * t - x;
}

/// Whites (@p whites true) or Blacks of @p value on ACEScc AP1 @p c.
inline auto ApplyAcescc(Rgb c, float value, bool whites) -> Rgb {
  if (value == 0.0f) return c;
  const float luma  = 0.272229f * c[0] + 0.674082f * c[1] + 0.053689f * c[2];
  const float x     = (luma - kAcesccMidGrey) * kAcesccB;
  const float s     = value * kEvPerUnit;
  const float shift = whites ? ShiftEv(x, s) : -ShiftEv(-x, -s);
  const float d     = shift / kAcesccB;
  return {c[0] + d, c[1] + d, c[2] + d};
}

}  // namespace alcedo::tone_zone_reference
