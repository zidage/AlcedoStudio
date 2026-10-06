//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/diffusion_filter_plan.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace alcedo {
namespace {

constexpr std::uint32_t kMaxBaseLevel = 12;

auto LevelExtent(ImageExtent canvas_extent, std::uint32_t level) -> ImageExtent {
  const auto scale = std::uint64_t{1} << level;
  return {static_cast<std::uint32_t>((canvas_extent.width + scale - 1) / scale),
          static_cast<std::uint32_t>((canvas_extent.height + scale - 1) / scale)};
}

}  // namespace

auto DiffusionCanvasExtent(Extent2D full_reference_extent) -> ImageExtent {
  const auto dims = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(full_reference_extent.width),
      static_cast<int>(full_reference_extent.height), kDiffusionCanvasMaxLongEdge);
  return {static_cast<std::uint32_t>(dims.width), static_cast<std::uint32_t>(dims.height)};
}

auto MakeDiffusionFilterLayout(ImageExtent canvas_extent, const DiffusionFilterShape& shape)
    -> DiffusionFilterLayout {
  if (canvas_extent.width == 0 || canvas_extent.height == 0) {
    throw std::invalid_argument("MakeDiffusionFilterLayout: canvas extent is empty");
  }

  const double short_side =
      static_cast<double>((std::min)(canvas_extent.width, canvas_extent.height));
  const double sigma_base =
      (std::max)(static_cast<double>(shape.base_sigma_fraction) * short_side, 1.0);
  const double sigma_max =
      (std::max)(static_cast<double>(shape.glow_radius) * short_side, sigma_base);

  DiffusionFilterLayout layout;
  layout.canvas_extent = canvas_extent;
  layout.base_level    = static_cast<std::uint32_t>(
      std::clamp(std::lround(std::log2(sigma_base)), 0L, static_cast<long>(kMaxBaseLevel)));
  const auto requested_levels =
      static_cast<std::uint32_t>(std::ceil(std::log2(sigma_max / sigma_base) - 1e-9)) + 1U;
  const auto max_levels = std::clamp(requested_levels, 1U, kDiffusionMaxLevels);

  // Stop at the first 1 x 1 level; coarser levels hold the same value.
  std::uint32_t level_count = 0;
  for (std::uint32_t index = 0; index < max_levels; ++index) {
    const auto extent           = LevelExtent(canvas_extent, layout.base_level + index);
    layout.extents[level_count] = extent;
    ++level_count;
    if (extent.width == 1 && extent.height == 1) {
      break;
    }
  }
  layout.level_count = level_count;

  double weight_sum = 0.0;
  for (std::uint32_t index = 0; index < level_count; ++index) {
    const double sigma = sigma_base * std::ldexp(1.0, static_cast<int>(index));
    const double position =
        level_count > 1 ? static_cast<double>(index) / static_cast<double>(level_count - 1) : 0.0;
    const double veil   = 1.0 - static_cast<double>(shape.black_mist) * position * position;
    const double weight = std::pow(sigma, 2.0 - static_cast<double>(shape.power_law_exponent)) *
                          (std::max)(veil, 0.0);
    layout.weights[index] = static_cast<float>(weight);
    weight_sum += weight;
  }
  // Level 0 has no veil reduction, so weight_sum is always positive.
  for (std::uint32_t index = 0; index < level_count; ++index) {
    layout.weights[index] = static_cast<float>(layout.weights[index] / weight_sum);
  }

  layout.scatter_fraction = shape.scatter_fraction;
  layout.transmission =
      1.0f - shape.black_absorption * shape.black_mist * shape.scatter_fraction;
  layout.highlight_gain = shape.highlight_glow;
  layout.highlight_low  = shape.highlight_low_stops;
  layout.highlight_high = shape.highlight_high_stops;
  return layout;
}

auto MakeDiffusionScatterMapping(const ResolvedRenderGeometry& geometry,
                                 const DiffusionFilterLayout&  layout)
    -> DiffusionScatterMapping {
  const auto reference = geometry.full_reference_extent;
  const auto base      = layout.extents[0];
  if (reference.Empty() || base.width == 0 || base.height == 0) {
    throw std::invalid_argument("MakeDiffusionScatterMapping: empty reference or base extent");
  }
  // The base level covers the full reference frame; texel edges are the frame edges.
  const auto reference_to_base =
      Matrix3x3::Scale(static_cast<float>(base.width) / static_cast<float>(reference.width),
                       static_cast<float>(base.height) / static_cast<float>(reference.height));
  const auto base_to_reference =
      Matrix3x3::Scale(static_cast<float>(reference.width) / static_cast<float>(base.width),
                       static_cast<float>(reference.height) / static_cast<float>(base.height));

  DiffusionScatterMapping mapping;
  mapping.render_to_base = reference_to_base * geometry.render_to_reference;
  mapping.base_to_render = geometry.reference_to_render * base_to_reference;

  // Render pixels covered by one base texel along each base axis.
  const auto& m          = mapping.base_to_render.m;
  const float footprint  = (std::max)(std::hypot(m[0], m[3]), std::hypot(m[1], m[4]));
  const float samples    = std::isfinite(footprint) ? std::ceil(footprint) : 1.0f;
  mapping.reduce_samples = static_cast<std::uint32_t>(
      std::clamp(samples, 1.0f, static_cast<float>(kDiffusionMaxReduceSamples)));
  return mapping;
}

}  // namespace alcedo
