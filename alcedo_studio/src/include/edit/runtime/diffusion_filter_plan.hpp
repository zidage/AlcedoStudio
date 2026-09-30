//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/runtime/content_key.hpp"

namespace alcedo {

/// Largest scatter pyramid. 16 levels cover every render extent up to 2^20 pixels per axis.
inline constexpr std::uint32_t kDiffusionMaxLevels = 16;

/**
 * @brief Scatter pyramid for one render frame. Backend-neutral; computed on the CPU per frame.
 *
 * Level `k` has texels of `2^(base_level + k)` render pixels. Level 0 (the base level) is the
 * highlight-boosted box reduction of the linear scene. Each coarser level is the 13-tap
 * downsample of the previous level. The scatter image is
 * `sum_k weights[k] * level_k`, accumulated from the coarsest level with 9-tap tent upsamples.
 *
 * The output pixel is `transmission * ((1 - scatter_fraction) * I + scatter_fraction * B)`,
 * where `B` is the scatter image sampled at the render pixel. Weights sum to 1, so the scatter
 * conserves the energy of the boosted input.
 */
struct DiffusionFilterLayout {
  std::uint32_t                                base_level  = 0;
  std::uint32_t                                level_count = 0;
  std::array<ImageExtent, kDiffusionMaxLevels> extents{};
  std::array<float, kDiffusionMaxLevels>       weights{};
  float                                        scatter_fraction = 0.0f;
  float                                        transmission     = 1.0f;
  float                                        highlight_gain   = 0.0f;
  float                                        highlight_knee   = 0.8f;
};

/**
 * @brief Short side of the full reference image, in render pixels.
 *
 * The filter scatters light over a fraction of the whole sensor frame, so preview, detail, and
 * export renders use the same image-space glow size.
 */
[[nodiscard]] auto DiffusionShortSideRenderPixels(const ResolvedRenderGeometry& geometry) -> float;

/**
 * @brief Build the scatter pyramid for @p shape at @p render_extent.
 *
 * @param render_extent Extent of the scene image that the filter reads and writes.
 * @param short_side_render_pixels Result of @ref DiffusionShortSideRenderPixels.
 * @throws std::invalid_argument for an empty extent or a non-positive short side.
 */
[[nodiscard]] auto MakeDiffusionFilterLayout(ImageExtent                 render_extent,
                                             float                       short_side_render_pixels,
                                             const DiffusionFilterShape& shape)
    -> DiffusionFilterLayout;

}  // namespace alcedo
