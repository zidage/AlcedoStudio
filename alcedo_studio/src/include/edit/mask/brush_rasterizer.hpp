//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_coverage_update.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Owns one current Brush source R8 and stamps canonical dabs in evaluation order.
 *
 * Regional replay initializes the dirty rectangle to 0 and restamps intersecting
 * spans. It does not keep a per-stroke raster checkpoint. Geometry is the canonical
 * Brush grid from @ref CanonicalBrushRasterExtent; zoom and DPR must not be passed.
 *
 * Thread: serial owner worker. Not thread-safe.
 */
class BrushRasterizer {
 public:
  /**
   * @brief Allocate a zeroed canonical raster.
   *
   * @throws std::runtime_error when @p raster or @p full_reference is empty.
   */
  void SetGeometry(Extent2D raster, Extent2D full_reference);

  /**
   * @brief Fill every texel with 0. No-op until @ref SetGeometry.
   */
  void ClearToZero();

  /**
   * @brief Rebuild the entire source from @p source, starting from 0.
   *
   * @throws std::runtime_error when geometry is unset, the algorithm version is not 1,
   *         or a stroke is invalid.
   */
  void RasterizeFull(const BrushMaskSource& source);

  /**
   * @brief Rebuild @p dirty from b0 = 0 using ordered index spans of @p source.
   *
   * Texels outside @p dirty are left unchanged. An empty clipped dirty rectangle is
   * a no-op.
   *
   * @throws std::runtime_error when geometry is unset, @p index was built for a
   *         different raster, the algorithm version is not 1, or a span identity
   *         does not match @p source.
   */
  void ReplayRegion(const BrushMaskSource& source, const BrushSpatialIndex& index, RectI dirty);

  /**
   * @brief Stamp @p samples in order onto existing pixels. Does not zero any texel.
   *
   * Empty @p samples is a no-op. @p clip limits which texels may change.
   *
   * @throws std::runtime_error when geometry is unset.
   */
  void StampOrderedSamples(std::span<const BrushCanonicalSample> samples, Vector2 translation,
                           BrushStrokeMode mode, RectI clip);

  /**
   * @brief Apply a classified coverage update. @c StampNewSamples does not zero texels.
   *
   * @c ReplayDirty uses @p index over @c dirty. @c Unchanged is a no-op.
   */
  void ApplyCoverageUpdate(const BrushMaskSource& source, const BrushSpatialIndex& index,
                           const BrushCoverageUpdate& update);

  [[nodiscard]] auto Pixels() const -> std::span<const std::uint8_t> { return *pixels_; }
  /**
   * @brief Shared handle to the retained pixel buffer.
   *
   * The rasterizer keeps mutating it; later replay writes are visible through
   * the handle. Consumers treat the pointee as immutable during a render.
   */
  [[nodiscard]] auto SharedPixels() const -> std::shared_ptr<const std::vector<std::uint8_t>> {
    return pixels_;
  }
  [[nodiscard]] auto Raster() const -> Extent2D { return raster_; }
  [[nodiscard]] auto FullReference() const -> Extent2D { return full_reference_; }
  [[nodiscard]] auto Empty() const -> bool { return pixels_->empty(); }

 private:
  void RequireGeometry() const;
  void StampDab(const BrushCanonicalSample& sample, Vector2 translation, BrushStrokeMode mode,
                RectI clip);

  Extent2D                                  raster_{};
  Extent2D                                  full_reference_{};
  std::shared_ptr<std::vector<std::uint8_t>> pixels_ =
      std::make_shared<std::vector<std::uint8_t>>();
};

}  // namespace alcedo
