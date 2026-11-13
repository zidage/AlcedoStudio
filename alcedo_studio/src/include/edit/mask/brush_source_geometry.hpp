//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/// Default spatial-index tile size in canonical raster texels. Pixel values do not depend on it.
inline constexpr std::uint32_t kDefaultBrushSpatialIndexTileTexels = 64;

/**
 * @brief True when @p rect has no texels (non-positive width or height).
 */
[[nodiscard]] inline auto RectIEmpty(RectI rect) -> bool {
  return rect.width <= 0 || rect.height <= 0;
}

/**
 * @brief Tightly packed row-major R8 index for @p x, @p y in @p extent.
 *
 * @throws std::invalid_argument when @p extent is empty or the texel is outside it.
 */
[[nodiscard]] auto PackedR8Index(std::uint32_t x, std::uint32_t y, Extent2D extent) -> std::size_t;

/**
 * @brief Integer rectangle covering the entire raster, or empty when @p extent is empty.
 */
[[nodiscard]] auto FullTexelRect(Extent2D extent) -> RectI;

/**
 * @brief Clip @p rect to packed-R8 texel bounds. Empty when the intersection has no samples.
 */
[[nodiscard]] auto ClipTexelRect(RectI rect, Extent2D extent) -> RectI;

/**
 * @brief Bounding union of two texel rectangles. An empty operand is ignored.
 */
[[nodiscard]] auto UnionTexelRect(RectI a, RectI b) -> RectI;

/**
 * @brief Intersection of two texel rectangles. Empty when they do not overlap.
 */
[[nodiscard]] auto IntersectTexelRect(RectI a, RectI b) -> RectI;

/**
 * @brief Axis-aligned texel support of one dab after @p translation, clipped to @p raster.
 *
 * Uses the reference-pixel disk of @p sample.radius around the translated center.
 * Conservative AABB; stamping still tests Euclidean distance per texel.
 *
 * @throws std::invalid_argument when extents are empty or @p sample fails encoding rules.
 */
[[nodiscard]] auto BrushDabOutputTexelSupport(const BrushCanonicalSample& sample,
                                              Vector2 translation, Extent2D raster,
                                              Extent2D full_reference) -> RectI;

/**
 * @brief Dab support in local (untranslated) canonical texels.
 */
[[nodiscard]] auto BrushDabLocalTexelSupport(const BrushCanonicalSample& sample, Extent2D raster,
                                             Extent2D full_reference) -> RectI;

/**
 * @brief Union of dab supports for one stroke in output texels.
 */
[[nodiscard]] auto BrushStrokeOutputTexelSupport(const BrushStroke& stroke, Vector2 translation,
                                                 Extent2D raster, Extent2D full_reference)
    -> RectI;

/**
 * @brief Union of every dab in @p source after placement, clipped to @p raster.
 *
 * Does not expand for source feather or Mask invert. Those belong to effective coverage.
 *
 * @throws std::invalid_argument when extents are empty.
 */
[[nodiscard]] auto BrushSourceOutputTexelSupport(const BrushMaskSource& source, Extent2D raster,
                                                 Extent2D full_reference) -> RectI;

/**
 * @brief Convert an output-texel query into local-texel space by subtracting placement.
 *
 * Corners are mapped through reference pixels and outward-rounded so a sub-texel
 * translation cannot drop a contributing dab.
 *
 * @throws std::invalid_argument when extents are empty or @p translation is not finite.
 */
[[nodiscard]] auto BrushOutputTexelsToLocal(RectI output_texels, Vector2 translation,
                                            Extent2D raster, Extent2D full_reference) -> RectI;

/**
 * @brief Effective Mix support of one Mask, including invert and Brush feather expansion.
 *
 * Disabled Masks have empty support. Invert, Linear Gradient, and Brush feather > 0
 * use the full raster. Radial uses the outer-ellipse AABB. Brush without feather uses
 * dab supports.
 *
 * @throws std::invalid_argument when extents are empty.
 */
[[nodiscard]] auto EffectiveMaskTexelSupport(const MaskModel& mask, Extent2D raster,
                                             Extent2D full_reference) -> RectI;

}  // namespace alcedo
