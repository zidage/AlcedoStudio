//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

/**
 * @file oklab_contrast_reference.hpp
 * @brief Host reference for the Color Grade Contrast adjustment.
 *
 * The CUDA, Metal, and OpenCL primary-grade kernels implement the same math; GPU tests compare
 * their output against these functions.
 *
 * Contrast operates on OkLab lightness of scene-linear AP1:
 *   1. ACEScc -> linear AP1.
 *   2. AP1 -> LMS with a white-normalized matrix (each row sums to 1, a von Kries adaptation in
 *      OkLab LMS), so AP1 neutrals map to a = b = 0 and L = cbrt(Y) exactly.
 *   3. Stops from 18% grey: x = log2(L^3 / 0.18) = 3 * log2(L / cbrt(0.18)).
 *   4. Soft S curve in stops: f(x) = x + (k - 1) * w * tanh(x / w), with k = 2^(contrast / 100)
 *      and w = 2.5 stops. Slope is k at mid grey and returns to 1 far from it, so the shift is
 *      bounded by +/-(k - 1) * w stops. In lightness this is the gain
 *      L' = L * 2^((k - 1) * w * tanh(x / w) / 3); L <= 0 uses the tanh = -1 limit.
 *   5. Chroma follows contrast gently: a and b scale by sqrt(k) * min(gain, 1), where gain = L'/L.
 *      Where the curve darkens, chroma shrinks with lightness so saturation (C / L) rises by only
 *      sqrt(k); fixed chroma there would push noisy near-black colors out of gamut. Hue is
 *      unchanged.
 */
namespace alcedo::oklab_contrast_reference {

/// ACEScc-encoded or linear AP1 RGB, depending on the function.
using Rgb                               = std::array<float, 3>;

/// Luminance of the contrast pivot (18% grey), which the curve leaves unchanged.
inline constexpr float kPivotLuminance  = 0.18f;
/// Width w of the tanh S curve, in stops of luminance.
inline constexpr float kCurveWidthStops = 2.5f;

/// ACEScc encode, matching the grade working space of the GPU runtimes.
inline auto            AcesccEncode(float value) -> float {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) return kFloor + value;
  if (value < kTransition) return (std::log2(kOffset + value * 0.5f) + kA) / kB;
  return (std::log2(value) + kA) / kB;
}

inline auto AcesccDecode(float value) -> float {
  constexpr float kA         = 9.72f;
  constexpr float kB         = 17.52f;
  constexpr float kOffset    = 0.0000152587890625f;
  constexpr float kFloor     = (-16.0f + kA) / kB;
  constexpr float kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (std::exp2(value * kB - kA) - kOffset) * 2.0f;
  return std::exp2(value * kB - kA);
}

inline auto LinearAp1ToOkLab(const Rgb& c) -> Rgb {
  const float l  = 0.6341104672f * c[0] + 0.3489495087f * c[1] + 0.0169400240f * c[2];
  const float m  = 0.2754060131f * c[0] + 0.6327713632f * c[1] + 0.0918226237f * c[2];
  const float s  = 0.1056775254f * c[0] + 0.1971481306f * c[1] + 0.6971743440f * c[2];
  const float l_ = std::cbrt(l);
  const float m_ = std::cbrt(m);
  const float s_ = std::cbrt(s);
  return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
          1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
          0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_};
}

inline auto OkLabToLinearAp1(const Rgb& lab) -> Rgb {
  const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
  const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
  const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return {2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
          -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
          -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s};
}

/// Slider value (-100..100) to the mid-grey slope in stops.
inline auto ContrastSlope(float contrast) -> float { return std::exp2(contrast * 0.01f); }

/// Lightness gain L'/L for OkLab lightness @p lightness at slope @p slope.
inline auto LightnessGain(float lightness, float slope) -> float {
  const float pivot_lightness = std::cbrt(kPivotLuminance);
  const float shape =
      lightness > 0.0f ? std::tanh(3.0f * std::log2(lightness / pivot_lightness) / kCurveWidthStops)
                       : -1.0f;
  return std::exp2((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
}

inline auto ApplyContrastLinearAp1(const Rgb& ap1, float contrast) -> Rgb {
  if (contrast == 0.0f) return ap1;
  const float slope        = ContrastSlope(contrast);
  const Rgb   lab          = LinearAp1ToOkLab(ap1);
  const float gain         = LightnessGain(lab[0], slope);
  const float chroma_scale = std::sqrt(slope) * std::fmin(gain, 1.0f);
  return OkLabToLinearAp1({lab[0] * gain, lab[1] * chroma_scale, lab[2] * chroma_scale});
}

inline auto ApplyContrastAcescc(const Rgb& acescc, float contrast) -> Rgb {
  if (contrast == 0.0f) return acescc;
  const Rgb out = ApplyContrastLinearAp1(
      {AcesccDecode(acescc[0]), AcesccDecode(acescc[1]), AcesccDecode(acescc[2])}, contrast);
  return {AcesccEncode(out[0]), AcesccEncode(out[1]), AcesccEncode(out[2])};
}

/// Largest linear-AP1 channel difference between two ACEScc pixels, relative to the brightest
/// linear channel of @p expected_acescc. An absolute ACEScc tolerance is not meaningful for a
/// channel many stops darker than the rest of its pixel: float cancellation in the OkLab -> AP1
/// matrix sets the error there, and the log encoding magnifies it.
inline auto RelativeLinearError(const Rgb& actual_acescc, const Rgb& expected_acescc) -> float {
  float scale = 1.0e-6f;
  for (const float channel : expected_acescc) {
    scale = std::fmax(scale, std::fabs(AcesccDecode(channel)));
  }
  float error = 0.0f;
  for (std::size_t i = 0; i < actual_acescc.size(); ++i) {
    error = std::fmax(error,
                      std::fabs(AcesccDecode(actual_acescc[i]) - AcesccDecode(expected_acescc[i])));
  }
  return error / scale;
}

}  // namespace alcedo::oklab_contrast_reference
