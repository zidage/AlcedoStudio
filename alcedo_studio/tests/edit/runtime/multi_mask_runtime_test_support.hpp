//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo::multi_mask_test {

/** @brief R8 comparisons allow at most one code value. */
inline constexpr int kR8ToleranceCodes = 1;

/**
 * @brief Round-half-up R8 encoding used by CUDA, OpenCL, and Metal coverage writes.
 *
 * `code = clamp(value * 255 + 0.5, 0, 255)` as uint8.
 */
inline auto QuantizeR8(float value) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
}

/**
 * @brief Invert, then opacity, then clamp. Range fields are identity (1) in this runtime.
 */
inline auto ApplyInvertAndOpacity(float coverage, bool invert, float opacity) -> float {
  if (invert) {
    coverage = 1.0f - coverage;
  }
  return std::clamp(coverage * opacity, 0.0f, 1.0f);
}

inline auto Transform(const Matrix3x3& matrix, float x, float y) -> Vector2 {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

inline auto RadialSourceCoverage(const RadialMaskSource& radial, float nx, float ny) -> float {
  const float c      = std::cos(radial.rotation);
  const float s      = std::sin(radial.rotation);
  const float dx     = nx - radial.center_x;
  const float dy     = ny - radial.center_y;
  const float rx     = (c * dx + s * dy) / std::max(radial.major_radius, 1.0e-6f);
  const float ry     = (-s * dx + c * dy) / std::max(radial.minor_radius, 1.0e-6f);
  const float radius = std::sqrt(rx * rx + ry * ry);
  const float inner  = std::max(0.0f, 1.0f - radial.inner_feather);
  const float outer  = 1.0f + radial.outer_feather;
  return 1.0f - std::clamp((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f, 1.0f);
}

inline auto LinearGradientSourceCoverage(const LinearGradientMaskSource& gradient, float nx,
                                         float ny) -> float {
  const float length   = std::hypot(gradient.normal_x, gradient.normal_y);
  const float normal_x = gradient.normal_x / std::max(length, 1.0e-6f);
  const float normal_y = gradient.normal_y / std::max(length, 1.0e-6f);
  const float distance =
      (nx - gradient.origin_x) * normal_x + (ny - gradient.origin_y) * normal_y;
  const float t =
      std::clamp(distance / std::max(gradient.transition_distance, 1.0e-6f) + 0.5f, 0.0f, 1.0f);
  return gradient.start_value + (gradient.end_value - gradient.start_value) * t;
}

inline auto NormalizedReference(const ResolvedRenderGeometry& geometry, std::uint32_t x,
                                std::uint32_t y) -> Vector2 {
  const auto reference =
      Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
  return {reference.x / static_cast<float>(geometry.full_reference_extent.width),
          reference.y / static_cast<float>(geometry.full_reference_extent.height)};
}

/** @brief Effective coverage of one analytic Mask at a render pixel. Disabled Masks are 0. */
inline auto EffectiveCoverageAt(const MaskModel& mask,
                                const ResolvedRenderGeometry& geometry, std::uint32_t x,
                                std::uint32_t y) -> float {
  if (!mask.enabled) {
    return 0.0f;
  }
  float source = 0.0f;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const auto n = NormalizedReference(geometry, x, y);
    source       = RadialSourceCoverage(*radial, n.x, n.y);
  } else if (const auto* gradient = std::get_if<LinearGradientMaskSource>(&mask.source)) {
    const auto n = NormalizedReference(geometry, x, y);
    source       = LinearGradientSourceCoverage(*gradient, n.x, n.y);
  }
  return ApplyInvertAndOpacity(source, mask.invert, mask.opacity);
}

/**
 * @brief Maximum of enabled Masks. An empty list is not a Mask image (Grade coverage 1).
 *
 * A nonempty all-disabled list is zero coverage.
 */
inline auto EvaluateEnabledUnionR8(std::span<const MaskModel> masks,
                                   const ResolvedRenderGeometry& geometry)
    -> std::vector<std::uint8_t> {
  const auto width  = geometry.render_extent.width;
  const auto height = geometry.render_extent.height;
  std::vector<std::uint8_t> output(static_cast<std::size_t>(width) * height, 0);
  bool                      any_enabled = false;
  for (const auto& mask : masks) {
    any_enabled = any_enabled || mask.enabled;
  }
  if (!any_enabled) {
    return output;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      float coverage = 0.0f;
      for (const auto& mask : masks) {
        coverage = std::max(coverage, EffectiveCoverageAt(mask, geometry, x, y));
      }
      output[static_cast<std::size_t>(y) * width + x] = QuantizeR8(coverage);
    }
  }
  return output;
}

inline void ExpectR8WithinTolerance(std::span<const std::uint8_t> actual,
                                    std::span<const std::uint8_t> expected,
                                    int tolerance = kR8ToleranceCodes) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_NEAR(actual[i], expected[i], tolerance) << "index " << i;
  }
}

}  // namespace alcedo::multi_mask_test
