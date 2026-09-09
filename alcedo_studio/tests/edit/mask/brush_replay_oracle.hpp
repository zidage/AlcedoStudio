//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>
#include <vector>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo::brush_replay_oracle {

inline constexpr float kAnalyticEpsilon = 1.0e-6f;

inline auto MakeZeroR8(Extent2D extent) -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(static_cast<std::size_t>(extent.width) * extent.height, 0);
}

inline auto CombineDab(std::uint8_t previous, std::uint8_t dab, BrushStrokeMode mode)
    -> std::uint8_t {
  return mode == BrushStrokeMode::Erase ? EraseBrushR8(previous, dab) : PaintBrushR8(previous, dab);
}

/**
 * @brief Independent full Brush source R8. Walks every sample at every texel.
 *
 * Does not use the spatial index or regional rasterizer.
 */
inline auto BrushSourceR8(const BrushMaskSource& source, Extent2D raster, Extent2D full_reference)
    -> std::vector<std::uint8_t> {
  auto pixels = MakeZeroR8(raster);
  for (const auto& stroke : source.strokes) {
    for (const auto& sample : BrushStrokeSamples(stroke)) {
      const float world_x = sample.local_x + source.placement_translation.x;
      const float world_y = sample.local_y + source.placement_translation.y;
      for (std::uint32_t y = 0; y < raster.height; ++y) {
        for (std::uint32_t x = 0; x < raster.width; ++x) {
          const auto  center = CanonicalBrushTexelReferenceCenter(x, y, raster, full_reference);
          const float distance = std::hypot(center.x - world_x, center.y - world_y);
          const auto  dab      = QuantizeMaskCoverageToR8(
              BrushDabCoverage(distance, sample.radius, sample.strength, sample.hardness));
          const auto index = PackedR8Index(x, y, raster);
          pixels[index]    = CombineDab(pixels[index], dab, stroke.mode);
        }
      }
    }
  }
  return pixels;
}

/**
 * @brief Exhaustive signed-distance feather. Independent of the separable DT.
 */
inline auto ExactSignedDistanceFeather(std::span<const std::uint8_t> source, Extent2D extent,
                                       float radius_texels, bool invert, float opacity)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> output(source.size());
  for (std::uint32_t y = 0; y < extent.height; ++y) {
    for (std::uint32_t x = 0; x < extent.width; ++x) {
      const auto  index    = PackedR8Index(x, y, extent);
      const float coverage = CoverageFromMaskR8(source[index]);
      const bool  inside   = coverage >= 0.5f;
      float       signed_distance;
      if (coverage > 0.0f && coverage < 1.0f) {
        signed_distance = coverage - 0.5f;
      } else {
        float nearest_squared = (std::numeric_limits<float>::max)();
        for (std::uint32_t ty = 0; ty < extent.height; ++ty) {
          for (std::uint32_t tx = 0; tx < extent.width; ++tx) {
            const auto t_index = PackedR8Index(tx, ty, extent);
            if ((source[t_index] >= 128) == inside) {
              continue;
            }
            const float dx  = static_cast<float>(x) - static_cast<float>(tx);
            const float dy  = static_cast<float>(y) - static_cast<float>(ty);
            nearest_squared = std::min(nearest_squared, dx * dx + dy * dy);
          }
        }
        const float exact       = std::sqrt(nearest_squared);
        const float to_boundary = std::max(exact - 0.5f, 0.0f);
        signed_distance         = inside ? to_boundary : -to_boundary;
      }
      float value = radius_texels <= 0.0f
                        ? (signed_distance >= 0.0f ? 1.0f : 0.0f)
                        : std::clamp(0.5f + signed_distance / (2.0f * radius_texels), 0.0f, 1.0f);
      value       = value * value * (3.0f - 2.0f * value);
      if (invert) {
        value = 1.0f - value;
      }
      output[index] = QuantizeMaskCoverageToR8(std::clamp(value * opacity, 0.0f, 1.0f));
    }
  }
  return output;
}

inline auto AnalyticCoverage(const MaskModel& mask, std::uint32_t x, std::uint32_t y,
                             Extent2D raster, Extent2D full_reference) -> float {
  const auto  center = CanonicalBrushTexelReferenceCenter(x, y, raster, full_reference);
  const float nx     = center.x / static_cast<float>(full_reference.width);
  const float ny     = center.y / static_cast<float>(full_reference.height);
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const float c     = std::cos(radial->rotation);
    const float s     = std::sin(radial->rotation);
    const float dx    = nx - radial->center_x;
    const float dy    = ny - radial->center_y;
    const float rx    = (c * dx + s * dy) / std::max(radial->major_radius, kAnalyticEpsilon);
    const float ry    = (-s * dx + c * dy) / std::max(radial->minor_radius, kAnalyticEpsilon);
    const float rho   = std::sqrt(rx * rx + ry * ry);
    const float inner = std::max(0.0f, 1.0f - radial->inner_feather);
    const float outer = 1.0f + radial->outer_feather;
    return 1.0f - std::clamp((rho - inner) / std::max(outer - inner, kAnalyticEpsilon), 0.0f, 1.0f);
  }
  const auto& linear        = std::get<LinearGradientMaskSource>(mask.source);
  const float normal_length = std::hypot(linear.normal_x, linear.normal_y);
  const float normal_x      = linear.normal_x / std::max(normal_length, kAnalyticEpsilon);
  const float normal_y      = linear.normal_y / std::max(normal_length, kAnalyticEpsilon);
  const float distance      = (nx - linear.origin_x) * normal_x + (ny - linear.origin_y) * normal_y;
  const float t =
      std::clamp(distance / std::max(linear.transition_distance, kAnalyticEpsilon) + 0.5f, 0.0f,
                 1.0f);
  return linear.start_value + (linear.end_value - linear.start_value) * t;
}

inline auto EffectiveMaskR8(const MaskModel& mask, Extent2D raster, Extent2D full_reference)
    -> std::vector<std::uint8_t> {
  auto pixels = MakeZeroR8(raster);
  if (!mask.enabled) {
    return pixels;
  }
  if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
    auto source = BrushSourceR8(*brush, raster, full_reference);
    if (brush->feather_radius > 0.0f) {
      const auto radius_texels = BrushFeatherRadiusToSourceTexels(
          brush->feather_radius, raster, CanonicalBrushReferenceBounds(), full_reference);
      return ExactSignedDistanceFeather(source, raster, radius_texels, mask.invert, mask.opacity);
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
      float coverage = CoverageFromMaskR8(source[i]);
      if (mask.invert) {
        coverage = 1.0f - coverage;
      }
      pixels[i] = QuantizeMaskCoverageToR8(std::clamp(coverage * mask.opacity, 0.0f, 1.0f));
    }
    return pixels;
  }
  for (std::uint32_t y = 0; y < raster.height; ++y) {
    for (std::uint32_t x = 0; x < raster.width; ++x) {
      float coverage = AnalyticCoverage(mask, x, y, raster, full_reference);
      if (mask.invert) {
        coverage = 1.0f - coverage;
      }
      pixels[PackedR8Index(x, y, raster)] =
          QuantizeMaskCoverageToR8(std::clamp(coverage * mask.opacity, 0.0f, 1.0f));
    }
  }
  return pixels;
}

inline auto GradeMixR8(std::span<const MaskModel> masks, Extent2D raster, Extent2D full_reference)
    -> std::vector<std::uint8_t> {
  if (masks.empty()) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(raster.width) * raster.height, 255);
  }
  auto       mix         = MakeZeroR8(raster);
  bool       any_enabled = false;
  for (const auto& mask : masks) {
    if (!mask.enabled) {
      continue;
    }
    any_enabled     = true;
    const auto one  = EffectiveMaskR8(mask, raster, full_reference);
    for (std::size_t i = 0; i < mix.size(); ++i) {
      mix[i] = mix[i] > one[i] ? mix[i] : one[i];
    }
  }
  if (!any_enabled) {
    return mix;
  }
  return mix;
}

}  // namespace alcedo::brush_replay_oracle
