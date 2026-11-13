//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_range_display.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace alcedo {
namespace {

constexpr float    kPi = 3.14159265358979323846f;

/// Inverse of `LinearAp1ToOkLab` of the MaskEvaluate and Contrast kernels
/// (`cuda_primary_grade_pass.cu`, `primary_grade.cl`, `primary_grade.metal`).
[[nodiscard]] auto OkLabToLinearAp1(float lightness, float a, float b) -> std::array<float, 3> {
  const float l_ = lightness + 0.3963377774f * a + 0.2158037573f * b;
  const float m_ = lightness - 0.1055613458f * a - 0.0638541728f * b;
  const float s_ = lightness - 0.0894841775f * a - 1.2914855480f * b;
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return {2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
          -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
          -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s};
}

/// ACES AP1 to linear Rec.709 primaries, Bradford D60 to D65; each row sums to 1.
[[nodiscard]] auto LinearAp1ToLinearRec709(const std::array<float, 3>& ap1)
    -> std::array<float, 3> {
  return {1.7050509927f * ap1[0] - 0.6217921207f * ap1[1] - 0.0832588720f * ap1[2],
          -0.1302564175f * ap1[0] + 1.1408047366f * ap1[1] - 0.0105483191f * ap1[2],
          -0.0240033568f * ap1[0] - 0.1289689761f * ap1[1] + 1.1529723329f * ap1[2]};
}

[[nodiscard]] auto EncodeSrgb(float linear) -> float {
  const float clipped = std::clamp(linear, 0.0f, 1.0f);
  return clipped <= 0.0031308f ? 12.92f * clipped
                               : 1.055f * std::pow(clipped, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

auto MaskHueBarColor(float hue_degrees) -> std::array<float, 3> {
  const float radians = hue_degrees * kPi / 180.0f;
  const auto  ap1    = OkLabToLinearAp1(kMaskHueBarLightness, kMaskHueBarChroma * std::cos(radians),
                                        kMaskHueBarChroma * std::sin(radians));
  const auto  linear = LinearAp1ToLinearRec709(ap1);
  return {EncodeSrgb(linear[0]), EncodeSrgb(linear[1]), EncodeSrgb(linear[2])};
}

auto MaskLuminanceBarGray(float lightness) -> float {
  const float clipped = std::clamp(lightness, 0.0f, 1.0f);
  return EncodeSrgb(clipped * clipped * clipped);
}

}  // namespace alcedo
