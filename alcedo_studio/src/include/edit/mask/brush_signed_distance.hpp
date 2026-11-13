//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief Full-field signed-distance Brush feather matching the native Mask pass.
 *
 * Separable Euclidean distance is computed over the entire source. This is not a
 * local tile update: a small R8 dab dirty rectangle is not the feather domain.
 * Scratch buffers are reused for the next call and are not per-stroke caches.
 *
 * Thread: serial owner worker. Not thread-safe.
 */
class BrushSignedDistanceFeather {
 public:
  /**
   * @brief Feather packed R8 @p source onto a same-extent R8 result.
   *
   * Compose uses coverage in `(0, 1)` as `coverage - 0.5`, otherwise the exact
   * distance to the opposite binary band minus 0.5. Smoothstep, invert, opacity,
   * and round-half-up follow the native Mask pass.
   *
   * @param source Tightly packed R8, size `extent.width * extent.height`.
   * @param extent Positive source/output extent.
   * @param radius_texels Nonnegative finite source-texel feather radius.
   * @param invert Applied after feather and before opacity.
   * @param opacity Multiplier in `[0, 1]` after invert.
   * @return New packed R8 buffer. @p source is not mutated.
   * @throws std::runtime_error when extents, byte count, or parameters are invalid.
   */
  [[nodiscard]] auto Apply(std::span<const std::uint8_t> source, Extent2D extent,
                           float radius_texels, bool invert, float opacity)
      -> std::vector<std::uint8_t>;

 private:
  void EnsureScratch(std::uint32_t width, std::uint32_t height);
  void BandHorizontal(std::span<const std::uint8_t> source, bool want_inside);
  void BandVertical(std::span<float> squared_distance);

  std::uint32_t      width_  = 0;
  std::uint32_t      height_ = 0;
  std::vector<float> horizontal_;
  std::vector<float> inside_;
  std::vector<float> outside_;
  std::vector<int>   sites_;
  std::vector<float> boundaries_;
};

}  // namespace alcedo
