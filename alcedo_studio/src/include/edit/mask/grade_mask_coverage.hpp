//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_rasterizer.hpp"
#include "edit/mask/brush_signed_distance.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Current Grade Mix coverage R8 from parameterized sources, without raster history.
 *
 * Owns one Mix buffer, derived Brush spatial indices, and transient source/feather
 * scratch. Empty Mask lists write 255; a nonempty list with no enabled Mask writes 0;
 * otherwise enabled Masks combine by per-pixel maximum after feather, invert, and
 * opacity. Regional replay rebuilds the dirty rectangle from that defined base.
 *
 * Thread: serial owner worker. Not thread-safe.
 */
class GradeMaskCoverage {
 public:
  /**
   * @brief Allocate a zeroed Mix raster on the canonical Brush grid.
   *
   * @throws std::runtime_error when @p raster or @p full_reference is empty.
   */
  void SetGeometry(Extent2D raster, Extent2D full_reference,
                   std::uint32_t tile_texels = kDefaultBrushSpatialIndexTileTexels);

  /**
   * @brief Rebuild the local-space spatial index for @p mask_id from @p source.
   *
   * Translation is not stored in the index. Call after stroke insert/remove, not
   * after placement-only edits.
   *
   * @throws std::runtime_error when geometry is unset or @p source cannot be indexed.
   */
  void BindBrushSource(const MaskId& mask_id, const BrushMaskSource& source);

  /**
   * @brief Drop the spatial index for @p mask_id. Missing ids are ignored.
   */
  void UnbindMask(const MaskId& mask_id);

  /**
   * @brief Evaluate every Mask into the Mix buffer from the defined base.
   *
   * @throws std::runtime_error when geometry is unset, a Brush is unbound, or
   *         evaluation fails.
   */
  void EvaluateFull(std::span<const MaskModel> masks);

  /**
   * @brief Rebuild Mix in @p dirty from the defined base using surviving sources.
   *
   * Empty lists and all-disabled lists fill the entire raster (those bases are
   * global). Brush feather > 0 still runs a complete signed-distance pass; the
   * Mix write is then the full raster.
   *
   * @throws std::runtime_error when geometry is unset or a Brush index is missing.
   */
  void ReplayRegion(std::span<const MaskModel> masks, RectI dirty);

  [[nodiscard]] auto Pixels() const -> std::span<const std::uint8_t> { return mix_; }
  [[nodiscard]] auto Raster() const -> Extent2D { return raster_; }
  [[nodiscard]] auto FullReference() const -> Extent2D { return full_reference_; }
  [[nodiscard]] auto BrushIndex(const MaskId& mask_id) const -> const BrushSpatialIndex*;

 private:
  void RequireGeometry() const;
  void FillMix(std::uint8_t value);
  void ReplayClippedRegion(std::span<const MaskModel> masks, RectI dirty);
  void WriteEffectiveBrush(const MaskModel& mask, const BrushMaskSource& brush, RectI region,
                           std::span<std::uint8_t> dest);
  void WriteEffectiveAnalytic(const MaskModel& mask, RectI region, std::span<std::uint8_t> dest);
  auto AnalyticCoverage(const MaskModel& mask, std::uint32_t x, std::uint32_t y) const -> float;

  Extent2D                         raster_{};
  Extent2D                         full_reference_{};
  std::uint32_t                    tile_texels_ = kDefaultBrushSpatialIndexTileTexels;
  std::vector<std::uint8_t>        mix_;
  std::vector<std::uint8_t>        scratch_;
  BrushRasterizer                  rasterizer_;
  BrushSignedDistanceFeather       feather_;
  std::map<MaskId, BrushSpatialIndex> brush_indices_;
};

}  // namespace alcedo
