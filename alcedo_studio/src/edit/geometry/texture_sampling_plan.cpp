//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/geometry/texture_sampling_plan.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace alcedo {
namespace {

auto ClampBounds(NormalizedRect bounds) -> NormalizedRect {
  if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.w) ||
      !std::isfinite(bounds.h)) {
    throw std::runtime_error("MakeRasterMaskSamplingPlan: bounds must be finite");
  }
  bounds.w = std::clamp(bounds.w, kGeometryMinNormalizedSize, 1.0f);
  bounds.h = std::clamp(bounds.h, kGeometryMinNormalizedSize, 1.0f);
  bounds.x = std::clamp(bounds.x, 0.0f, 1.0f - bounds.w);
  bounds.y = std::clamp(bounds.y, 0.0f, 1.0f - bounds.h);
  return bounds;
}

auto ReferencePixelToUv(Extent2D full_reference, NormalizedRect bounds) -> Matrix3x3 {
  const float full_w = static_cast<float>(full_reference.width);
  const float full_h = static_cast<float>(full_reference.height);
  Matrix3x3   matrix;
  matrix.m[0] = 1.0f / (full_w * bounds.w);
  matrix.m[1] = 0.0f;
  matrix.m[2] = -bounds.x / bounds.w;
  matrix.m[3] = 0.0f;
  matrix.m[4] = 1.0f / (full_h * bounds.h);
  matrix.m[5] = -bounds.y / bounds.h;
  matrix.m[6] = 0.0f;
  matrix.m[7] = 0.0f;
  matrix.m[8] = 1.0f;
  return matrix;
}

}  // namespace

auto MakeRasterMaskSamplingPlan(const ResolvedRenderGeometry& geometry,
                                NormalizedRect reference_bounds, Extent2D texture_extent)
    -> TextureSamplingPlan {
  if (texture_extent.Empty()) {
    throw std::runtime_error("MakeRasterMaskSamplingPlan: texture_extent must be positive");
  }
  if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
    throw std::runtime_error("MakeRasterMaskSamplingPlan: geometry extents must be positive");
  }
  const auto bounds = ClampBounds(reference_bounds);
  TextureSamplingPlan plan;
  plan.render_to_texture_uv =
      ReferencePixelToUv(geometry.full_reference_extent, bounds) * geometry.render_to_reference;
  plan.uv_dx  = Vector2{plan.render_to_texture_uv.m[0], plan.render_to_texture_uv.m[3]};
  plan.uv_dy  = Vector2{plan.render_to_texture_uv.m[1], plan.render_to_texture_uv.m[4]};
  plan.filter = geometry.filter;

  const float tex_w = static_cast<float>(texture_extent.width);
  const float tex_h = static_cast<float>(texture_extent.height);
  const float rho_x = std::hypot(plan.uv_dx.x * tex_w, plan.uv_dx.y * tex_h);
  const float rho_y = std::hypot(plan.uv_dy.x * tex_w, plan.uv_dy.y * tex_h);
  plan.mip_level    = std::log2(std::max(std::max(rho_x, rho_y), 1.0e-6f));
  return plan;
}

auto MakeLlfSamplingPlan(const ResolvedRenderGeometry& geometry, Extent2D reference_texture_extent)
    -> TextureSamplingPlan {
  return MakeRasterMaskSamplingPlan(geometry, NormalizedRect{}, reference_texture_extent);
}

}  // namespace alcedo
