//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <variant>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"
#include "multi_mask_runtime_test_support.hpp"

/**
 * @file mask_range_reference.hpp
 * @brief Double-precision host reference of the Mask Luminance Range and Hue Range.
 *
 * The CUDA, OpenCL, and Metal MaskEvaluate kernels implement the same equations in float
 * (documented at alcedo::MaskRangeParams); GPU tests compare their R8 output against these
 * functions within one code value:
 *   1. ACEScc -> linear AP1 -> OkLab with the white-normalized AP1 -> LMS matrix of the Contrast
 *      operator (oklab_contrast_reference.hpp).
 *   2. L = clamp(OkLab L, 0, 1); C = hypot(a, b); h = atan2(b, a) in degrees [0, 360).
 *   3. luminance_weight = smoothstep(low - s, low, L) * (1 - smoothstep(high, high + s, L)).
 *   4. hue_weight = (1 - smoothstep(0, s, d)) * smoothstep(m - 0.015, m + 0.015, C), d = circular
 *      distance of h outside [hue_start, hue_end], m = min_chroma (default 0.03).
 *   5. coverage = clamp(invert(source) * hue_weight * luminance_weight * opacity, 0, 1).
 */
namespace alcedo::mask_range_reference {

/// One RGBA32F pixel of the Grade input: ACEScc-encoded AP1 and alpha.
struct ScenePixel {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

/// OkLab L, a, b of one pixel.
using Lab = std::array<double, 3>;

/// Every pixel of the synthetic scene has this extent (the AI Mask analysis extent).
inline constexpr Extent2D kSceneExtent{64, 48};

inline auto               AcesccDecode(double value) -> double {
  constexpr double kA         = 9.72;
  constexpr double kB         = 17.52;
  constexpr double kOffset    = 0.0000152587890625;
  constexpr double kFloor     = (-16.0 + kA) / kB;
  constexpr double kThreshold = (-15.0 + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (std::exp2(value * kB - kA) - kOffset) * 2.0;
  return std::exp2(value * kB - kA);
}

inline auto AcesccEncode(double value) -> double {
  constexpr double kA          = 9.72;
  constexpr double kB          = 17.52;
  constexpr double kOffset     = 0.0000152587890625;
  constexpr double kTransition = 0.000030517578125;
  constexpr double kFloor      = (-16.0 + kA) / kB;
  if (value < 0.0) return kFloor + value;
  if (value < kTransition) return (std::log2(kOffset + value * 0.5) + kA) / kB;
  return (std::log2(value) + kA) / kB;
}

inline auto LinearAp1ToOkLab(double r, double g, double b) -> Lab {
  const double l  = 0.6341104672 * r + 0.3489495087 * g + 0.0169400240 * b;
  const double m  = 0.2754060131 * r + 0.6327713632 * g + 0.0918226237 * b;
  const double s  = 0.1056775254 * r + 0.1971481306 * g + 0.6971743440 * b;
  const double l_ = std::cbrt(l);
  const double m_ = std::cbrt(m);
  const double s_ = std::cbrt(s);
  return {0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
          1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
          0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_};
}

inline auto OkLabToLinearAp1(const Lab& lab) -> std::array<double, 3> {
  const double l_ = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
  const double m_ = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
  const double s_ = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
  const double l  = l_ * l_ * l_;
  const double m  = m_ * m_ * m_;
  const double s  = s_ * s_ * s_;
  return {2.0693822907 * l - 1.1736821545 * m + 0.1042998638 * s,
          -0.8917480677 * l + 2.1537429245 * m - 0.2619948569 * s,
          -0.0615064732 * l - 0.4311325686 * m + 1.4926390418 * s};
}

/// OkLab of an ACEScc pixel.
inline auto SceneOkLab(const ScenePixel& pixel) -> Lab {
  return LinearAp1ToOkLab(AcesccDecode(pixel.r), AcesccDecode(pixel.g), AcesccDecode(pixel.b));
}

/// OkLCh hue in degrees, [0, 360).
inline auto HueDegrees(const Lab& lab) -> double {
  const double hue = std::atan2(lab[2], lab[1]) * 180.0 / std::numbers::pi;
  return hue < 0.0 ? hue + 360.0 : hue;
}

inline auto Chroma(const Lab& lab) -> double { return std::hypot(lab[1], lab[2]); }

/// ACEScc pixel of OkLCh (@p lightness, @p chroma, @p hue_degrees).
inline auto PixelFromOkLch(double lightness, double chroma, double hue_degrees) -> ScenePixel {
  const double radians = hue_degrees * std::numbers::pi / 180.0;
  const auto   linear =
      OkLabToLinearAp1({lightness, chroma * std::cos(radians), chroma * std::sin(radians)});
  return {static_cast<float>(AcesccEncode(linear[0])), static_cast<float>(AcesccEncode(linear[1])),
          static_cast<float>(AcesccEncode(linear[2])), 1.0f};
}

inline auto SmoothStep(double edge0, double edge1, double x) -> double {
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

/// Saturation gate of a Hue Range with threshold @p min_chroma at chroma @p chroma.
inline auto ChromaGate(double min_chroma, double chroma) -> double {
  return SmoothStep(min_chroma - 0.015, min_chroma + 0.015, chroma);
}

/// Hue Range weight of @p lab, including the chroma ramp.
inline auto HueWeight(const ColorRangeModel& range, const Lab& lab) -> double {
  const double start  = range.hue_start;
  const double end    = range.hue_end;
  const double width  = start <= end ? end - start : 360.0 - start + end;
  double       offset = HueDegrees(lab) - start;
  if (offset < 0.0) offset += 360.0;
  const double distance = offset <= width ? 0.0 : std::min(offset - width, 360.0 - offset);
  return (1.0 - SmoothStep(0.0, range.softness, distance)) *
         ChromaGate(range.min_chroma, Chroma(lab));
}

/// Luminance Range weight of @p lab.
inline auto LuminanceWeight(const LuminanceRangeModel& range, const Lab& lab) -> double {
  const double lightness = std::clamp(lab[0], 0.0, 1.0);
  const double low       = range.low;
  const double high      = range.high;
  const double softness  = range.softness;
  return SmoothStep(low - softness, low, lightness) *
         (1.0 - SmoothStep(high, high + softness, lightness));
}

/// Product of the restricted range weights of @p mask at @p pixel; 1 for a Mask without ranges.
inline auto RangeWeight(const MaskModel& mask, const ScenePixel& pixel) -> double {
  const Lab lab    = SceneOkLab(pixel);
  double    weight = 1.0;
  if (mask.color_range.has_value()) weight *= HueWeight(*mask.color_range, lab);
  if (mask.luminance_range.has_value()) weight *= LuminanceWeight(*mask.luminance_range, lab);
  return weight;
}

/// Round-half-up R8 code of the effective coverage of @p source with @p mask invert, opacity, and
/// range @p weight.
inline auto RangedCoverageR8(double source, const MaskModel& mask, double weight) -> std::uint8_t {
  if (mask.invert) source = 1.0 - source;
  const double value = std::clamp(source * weight * mask.opacity, 0.0, 1.0);
  return static_cast<std::uint8_t>(std::clamp(value * 255.0 + 0.5, 0.0, 255.0));
}

/// Expected R8 of an analytic @p mask with ranges on @p scene (row-major, render extent).
inline auto ExpectedAnalyticRangedR8(const MaskModel& mask, const ResolvedRenderGeometry& geometry,
                                     std::span<const ScenePixel> scene)
    -> std::vector<std::uint8_t> {
  const auto                width = geometry.render_extent.width;
  std::vector<std::uint8_t> output(scene.size());
  for (std::size_t index = 0; index < scene.size(); ++index) {
    const auto x      = static_cast<std::uint32_t>(index % width);
    const auto y      = static_cast<std::uint32_t>(index / width);
    const auto n      = multi_mask_test::NormalizedReference(geometry, x, y);
    double     source = 0.0;
    if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
      source = multi_mask_test::RadialSourceCoverage(*radial, n.x, n.y);
    } else if (const auto* gradient = std::get_if<LinearGradientMaskSource>(&mask.source)) {
      source = multi_mask_test::LinearGradientSourceCoverage(*gradient, n.x, n.y);
    }
    output[index] = RangedCoverageR8(source, mask, RangeWeight(mask, scene[index]));
  }
  return output;
}

/**
 * @brief ACEScc ramp and OkLCh hue wheel of @ref kSceneExtent.
 *
 * - Rows [0, 12): neutral ramp, OkLab L from 0 to 1.2 across x.
 * - Rows [12, 24): colored ramp, L from 0 to 1.2 across x, hue 15 degrees per row, chroma
 *   min(0.1 L, 0.08). Constant saturation keeps near-black pixels below the chroma ramp: the hue of
 *   a dark, strongly saturated color is not stable in float (cbrt of a near-zero LMS sum).
 * - Rows [24, 48): hue wheel, hue (x + 0.5) * 360 / 64, chroma from 0 to 0.13 down the rows (it
 *   crosses the 0.015 to 0.045 chroma ramp), L from 0.45 to 0.85 across alternate rows.
 */
inline auto MakeRampAndHueWheelScene() -> std::vector<ScenePixel> {
  const auto              width  = kSceneExtent.width;
  const auto              height = kSceneExtent.height;
  std::vector<ScenePixel> scene(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double u     = (x + 0.5) / width;
      auto&        pixel = scene[static_cast<std::size_t>(y) * width + x];
      if (y < 12) {
        pixel = PixelFromOkLch(1.2 * u, 0.0, 0.0);
      } else if (y < 24) {
        pixel = PixelFromOkLch(1.2 * u, std::min(0.12 * u, 0.08), 15.0 * (y - 12));
      } else {
        const double row = (y - 24) / 23.0;
        pixel            = PixelFromOkLch((y % 2 == 0) ? 0.45 : 0.85, 0.13 * row, 360.0 * u);
      }
    }
  }
  return scene;
}

/// Hue wheel only (rows [24, 48) of MakeRampAndHueWheelScene on every row): L in [0.45, 0.85].
inline auto MakeHueWheelScene() -> std::vector<ScenePixel> {
  const auto              width  = kSceneExtent.width;
  const auto              height = kSceneExtent.height;
  std::vector<ScenePixel> scene(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double u   = (x + 0.5) / width;
      const double row = y / (height - 1.0);
      scene[static_cast<std::size_t>(y) * width + x] =
          PixelFromOkLch((y % 2 == 0) ? 0.45 : 0.85, 0.13 * row, 360.0 * u);
    }
  }
  return scene;
}

}  // namespace alcedo::mask_range_reference
