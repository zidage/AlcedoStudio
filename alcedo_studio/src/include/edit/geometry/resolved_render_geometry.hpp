//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cmath>
#include <cstdint>

#include "edit/geometry/types.hpp"

namespace alcedo {

/**
 * @brief POD uniforms for GeometryResamplePass. No GPU resource types.
 */
struct GpuRenderGeometry {
  float         render_to_decoded[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t decoded_width        = 0;
  std::uint32_t decoded_height       = 0;
  std::uint32_t render_width         = 0;
  std::uint32_t render_height        = 0;
  float         border_rgba[4]       = {0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t filter               = 0;
};

/**
 * @brief Single rounding result for crop, rotation, view, and dynamic resolution.
 *
 * Image, raster-mask UV, and LLF must read this object. They must not re-round.
 */
struct ResolvedRenderGeometry {
  Extent2D decoded_extent{};
  Extent2D full_reference_extent{};
  Extent2D edit_extent{};
  Extent2D render_extent{};

  Matrix3x3 decoded_to_reference = Matrix3x3::Identity();
  Matrix3x3 reference_to_edit    = Matrix3x3::Identity();
  Matrix3x3 edit_to_render       = Matrix3x3::Identity();
  Matrix3x3 reference_to_render  = Matrix3x3::Identity();
  Matrix3x3 render_to_reference  = Matrix3x3::Identity();
  Matrix3x3 render_to_decoded    = Matrix3x3::Identity();

  RectI required_decoded_region{};
  RectI required_reference_region{};

  TextureFilter     filter = TextureFilter::Bilinear;
  GpuRenderGeometry gpu_data{};
};

/**
 * @brief True when render_to_decoded is identity and decoded extent equals render extent.
 *
 * GraphCompiler (G4+) may skip GeometryResamplePass when this is true.
 */
[[nodiscard]] inline auto IsIdentityResample(const ResolvedRenderGeometry& geometry) -> bool {
  return geometry.decoded_extent == geometry.render_extent &&
         IsApproxIdentity(geometry.render_to_decoded);
}

/**
 * @brief True when this render covers the whole EditSpace, not a viewport ROI.
 *
 * A full-view dynamic-resolution frame still returns true. A visible subrect returns
 * false. LLF uses this to decide whether the frame can seed the canonical reference.
 */
[[nodiscard]] inline auto CoversFullEditSpace(const ResolvedRenderGeometry& geometry) -> bool {
  if (geometry.edit_extent.Empty() || geometry.render_extent.Empty()) {
    return false;
  }
  const float det = geometry.edit_to_render.m[0] * geometry.edit_to_render.m[4] -
                    geometry.edit_to_render.m[1] * geometry.edit_to_render.m[3];
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
    return false;
  }
  const auto render_to_edit = InvertAffine(geometry.edit_to_render);
  const auto top_left       = TransformPoint(render_to_edit, {0.0f, 0.0f});
  const auto bottom_right   = TransformPoint(
      render_to_edit, {static_cast<float>(geometry.render_extent.width),
                       static_cast<float>(geometry.render_extent.height)});
  constexpr float kPixel = 1.5f;
  return top_left.x <= kPixel && top_left.y <= kPixel &&
         bottom_right.x >= static_cast<float>(geometry.edit_extent.width) - kPixel &&
         bottom_right.y >= static_cast<float>(geometry.edit_extent.height) - kPixel;
}

}  // namespace alcedo
