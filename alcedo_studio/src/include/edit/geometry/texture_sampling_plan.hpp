//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief How to sample a texture from RenderSpace. No GPU types.
 *
 * Raster masks: UV covers @p reference_bounds. Dynamic resolution updates this plan
 * without recreating the R8 texture (G6).
 */
struct TextureSamplingPlan {
  Matrix3x3     render_to_texture_uv = Matrix3x3::Identity();
  Vector2       uv_dx{};
  Vector2       uv_dy{};
  float         mip_level = 0.0f;
  TextureFilter filter    = TextureFilter::Bilinear;
};

/**
 * @brief Maps render pixel centers to UV over a ReferenceSpace rectangle.
 *
 * @param geometry Shared resolver result. Must not be re-rounded by the caller.
 * @param reference_bounds Normalized rect in ReferenceSpace covered by the texture.
 * @param texture_extent Texel size of the R8 (or LLF) texture. Used only for mip.
 */
[[nodiscard]] auto MakeRasterMaskSamplingPlan(const ResolvedRenderGeometry& geometry,
                                              NormalizedRect                reference_bounds,
                                              Extent2D texture_extent) -> TextureSamplingPlan;

/**
 * @brief LLF reference sampling: full-frame bounds, same render_to_reference as the image.
 */
[[nodiscard]] auto MakeLlfSamplingPlan(const ResolvedRenderGeometry& geometry,
                                       Extent2D                      reference_texture_extent)
    -> TextureSamplingPlan;

}  // namespace alcedo
