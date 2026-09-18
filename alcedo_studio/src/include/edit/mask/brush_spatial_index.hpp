//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief One indexed sample span: identities, evaluation range, and local texel bounds.
 *
 * Does not store sample coordinates, R8 pixels, or history. Rebuildable from the
 * parameterized Brush source.
 */
struct BrushIndexedSpan {
  StrokeId    stroke_id;
  std::size_t stroke_index = 0;
  std::size_t sample_begin = 0;
  std::size_t sample_end   = 0;
  RectI       local_bounds{};
};

/**
 * @brief Uniform-tile index of Brush sample spans in local canonical texels.
 *
 * Placement is applied at query time so a translation does not rebuild spans.
 * Tile size affects only which tiles are visited, not dab pixels.
 *
 * Thread: serial owner worker. Not thread-safe.
 */
class BrushSpatialIndex {
 public:
  /**
   * @brief Construct an empty index.
   *
   * @param tile_texels Positive tile size in canonical texels. Default is
   *        @ref kDefaultBrushSpatialIndexTileTexels.
   * @throws std::runtime_error when @p tile_texels is zero.
   */
  explicit BrushSpatialIndex(std::uint32_t tile_texels = kDefaultBrushSpatialIndexTileTexels);

  /**
   * @brief Replace spans from @p source. Translation is not baked into stored bounds.
   *
   * @throws std::runtime_error when extents are empty, the algorithm version is not 1,
   *         or a stroke is invalid.
   */
  void Rebuild(const BrushMaskSource& source, Extent2D raster, Extent2D full_reference);

  /**
   * @brief Drop all spans and geometry. Tile size is kept.
   */
  void Clear();

  [[nodiscard]] auto TileTexels() const -> std::uint32_t { return tile_texels_; }
  [[nodiscard]] auto Raster() const -> Extent2D { return raster_; }
  [[nodiscard]] auto FullReference() const -> Extent2D { return full_reference_; }
  [[nodiscard]] auto Empty() const -> bool { return spans_.empty(); }

  /**
   * @brief All stored spans in evaluation order. For tests and rebuild inspection.
   */
  [[nodiscard]] auto Spans() const -> std::span<const BrushIndexedSpan> { return spans_; }

  /**
   * @brief Spans whose local bounds intersect @p local_texels, in stroke/sample order.
   *
   * Each span appears once even when it overlaps several tiles. Empty query yields
   * an empty list, not an error.
   */
  [[nodiscard]] auto QueryLocal(RectI local_texels) const -> std::vector<BrushIndexedSpan>;

  /**
   * @brief Query in output texels by inverse-translating @p output_texels into local space.
   *
   * @throws std::runtime_error when geometry has not been rebuilt or @p translation is
   *         not finite.
   */
  [[nodiscard]] auto QueryOutput(RectI output_texels, Vector2 translation) const
      -> std::vector<BrushIndexedSpan>;

 private:
  auto TileIndex(std::uint32_t tile_x, std::uint32_t tile_y) const -> std::size_t;
  auto UniqueSpansIn(RectI local_texels) const -> std::vector<BrushIndexedSpan>;

  std::uint32_t                    tile_texels_ = kDefaultBrushSpatialIndexTileTexels;
  Extent2D                         raster_{};
  Extent2D                         full_reference_{};
  std::uint32_t                    tiles_x_ = 0;
  std::uint32_t                    tiles_y_ = 0;
  std::vector<BrushIndexedSpan>    spans_;
  std::vector<std::vector<std::size_t>> tile_span_indices_;
};

}  // namespace alcedo
