//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Current Grade Mix coverage R8 from analytic sources.
 *
 * Owns one Mix buffer and transient per-Mask scratch. Empty Mask lists write 255;
 * a nonempty list with no enabled Mask writes 0; otherwise enabled Masks combine
 * by per-pixel maximum after feather, invert, and opacity. Regional replay
 * rebuilds the dirty rectangle from that defined base.
 *
 * Thread: serial owner worker. Not thread-safe.
 */
class GradeMaskCoverage {
 public:
  /**
   * @brief Allocate a zeroed Mix raster over @p full_reference.
   *
   * @throws std::runtime_error when @p raster or @p full_reference is empty.
   */
  void SetGeometry(Extent2D raster, Extent2D full_reference);

  /**
   * @brief Evaluate every Mask into the Mix buffer from the defined base.
   *
   * @throws std::runtime_error when geometry is unset or evaluation fails.
   */
  void EvaluateFull(std::span<const MaskModel> masks);

  /**
   * @brief Rebuild Mix in @p dirty from the defined base using surviving sources.
   *
   * Empty lists and all-disabled lists fill the entire raster (those bases are
   * global).
   *
   * @throws std::runtime_error when geometry is unset or evaluation fails.
   */
  void ReplayRegion(std::span<const MaskModel> masks, RectI dirty);

  [[nodiscard]] auto Pixels() const -> std::span<const std::uint8_t> { return mix_; }
  [[nodiscard]] auto Raster() const -> Extent2D { return raster_; }
  [[nodiscard]] auto FullReference() const -> Extent2D { return full_reference_; }

 private:
  void RequireGeometry() const;
  void FillMix(std::uint8_t value);
  void ReplayClippedRegion(std::span<const MaskModel> masks, RectI dirty);
  void WriteEffectiveAnalytic(const MaskModel& mask, RectI region, std::span<std::uint8_t> dest);
  auto AnalyticCoverage(const MaskModel& mask, std::uint32_t x, std::uint32_t y) const -> float;

  Extent2D                  raster_{};
  Extent2D                  full_reference_{};
  std::vector<std::uint8_t> mix_;
  std::vector<std::uint8_t> scratch_;
};

}  // namespace alcedo
