//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/diffusion_filter_plan.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kMaxBaseLevel = 20;

auto LevelExtent(ImageExtent render_extent, std::uint32_t level) -> ImageExtent {
  const auto scale = std::uint64_t{1} << level;
  return {static_cast<std::uint32_t>((render_extent.width + scale - 1) / scale),
          static_cast<std::uint32_t>((render_extent.height + scale - 1) / scale)};
}

}  // namespace

auto DiffusionShortSideRenderPixels(const ResolvedRenderGeometry& geometry) -> float {
  const auto short_side = (std::min)(geometry.full_reference_extent.width,
                                     geometry.full_reference_extent.height);
  return static_cast<float>(short_side) * NeighborhoodRenderScale(geometry);
}

auto MakeDiffusionFilterLayout(ImageExtent render_extent, float short_side_render_pixels,
                               const DiffusionFilterShape& shape) -> DiffusionFilterLayout {
  if (render_extent.width == 0 || render_extent.height == 0) {
    throw std::invalid_argument("MakeDiffusionFilterLayout: render extent is empty");
  }
  if (!std::isfinite(short_side_render_pixels) || !(short_side_render_pixels > 0.0f)) {
    throw std::invalid_argument("MakeDiffusionFilterLayout: short side must be positive");
  }

  const double sigma_base = (std::max)(
      static_cast<double>(shape.base_sigma_fraction) * short_side_render_pixels, 1.0);
  const double sigma_max =
      (std::max)(static_cast<double>(shape.glow_radius) * short_side_render_pixels, sigma_base);

  DiffusionFilterLayout layout;
  layout.base_level = static_cast<std::uint32_t>(
      std::clamp(std::lround(std::log2(sigma_base)), 0L, static_cast<long>(kMaxBaseLevel)));
  const auto requested_levels =
      static_cast<std::uint32_t>(std::ceil(std::log2(sigma_max / sigma_base) - 1e-9)) + 1U;
  const auto max_levels = std::clamp(requested_levels, 1U, kDiffusionMaxLevels);

  // Stop at the first 1 x 1 level; coarser levels hold the same value.
  std::uint32_t level_count = 0;
  for (std::uint32_t index = 0; index < max_levels; ++index) {
    const auto extent            = LevelExtent(render_extent, layout.base_level + index);
    layout.extents[level_count]  = extent;
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
  layout.highlight_knee = shape.highlight_knee;
  return layout;
}

}  // namespace alcedo
