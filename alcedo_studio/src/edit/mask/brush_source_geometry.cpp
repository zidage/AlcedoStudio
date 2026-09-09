//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_source_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "edit/mask/active_raster_mask.hpp"

namespace alcedo {
namespace {

[[noreturn]] void FailGeometry(std::string_view message) {
  throw std::invalid_argument(std::string{message});
}

void RequirePositiveExtent(Extent2D extent, std::string_view name) {
  if (extent.Empty()) {
    FailGeometry(std::string{name} + " must be positive");
  }
}

void RequireFiniteTranslation(Vector2 translation) {
  if (!std::isfinite(translation.x) || !std::isfinite(translation.y)) {
    FailGeometry("placement_translation must be finite");
  }
}

auto DabSupport(const BrushCanonicalSample& sample, Vector2 translation, Extent2D raster,
                Extent2D full_reference) -> RectI {
  ValidateBrushCanonicalSample(sample);
  RequirePositiveExtent(raster, "raster");
  RequirePositiveExtent(full_reference, "full_reference");
  RequireFiniteTranslation(translation);
  const float scale_x = static_cast<float>(raster.width) / static_cast<float>(full_reference.width);
  const float scale_y =
      static_cast<float>(raster.height) / static_cast<float>(full_reference.height);
  const float world_x = sample.local_x + translation.x;
  const float world_y = sample.local_y + translation.y;
  const auto  x0 =
      static_cast<std::int32_t>(std::floor((world_x - sample.radius) * scale_x - 0.5f));
  const auto y0 =
      static_cast<std::int32_t>(std::floor((world_y - sample.radius) * scale_y - 0.5f));
  const auto x1 =
      static_cast<std::int32_t>(std::floor((world_x + sample.radius) * scale_x - 0.5f)) + 1;
  const auto y1 =
      static_cast<std::int32_t>(std::floor((world_y + sample.radius) * scale_y - 0.5f)) + 1;
  return ClipRasterDirtyRectangle({x0, y0, x1 - x0, y1 - y0}, raster);
}

}  // namespace

auto PackedR8Index(std::uint32_t x, std::uint32_t y, Extent2D extent) -> std::size_t {
  RequirePositiveExtent(extent, "extent");
  if (x >= extent.width || y >= extent.height) {
    FailGeometry("texel is outside the packed R8 raster");
  }
  return static_cast<std::size_t>(y) * extent.width + x;
}

auto FullTexelRect(Extent2D extent) -> RectI {
  if (extent.Empty()) {
    return {};
  }
  return {0, 0, static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)};
}

auto ClipTexelRect(RectI rect, Extent2D extent) -> RectI {
  return ClipRasterDirtyRectangle(rect, extent);
}

auto UnionTexelRect(RectI a, RectI b) -> RectI {
  if (RectIEmpty(a)) {
    return b;
  }
  if (RectIEmpty(b)) {
    return a;
  }
  const auto x0 = std::min(a.x, b.x);
  const auto y0 = std::min(a.y, b.y);
  const auto x1 = std::max(a.X1(), b.X1());
  const auto y1 = std::max(a.Y1(), b.Y1());
  return {x0, y0, x1 - x0, y1 - y0};
}

auto IntersectTexelRect(RectI a, RectI b) -> RectI {
  if (RectIEmpty(a) || RectIEmpty(b)) {
    return {};
  }
  const auto x0 = std::max(a.x, b.x);
  const auto y0 = std::max(a.y, b.y);
  const auto x1 = std::min(a.X1(), b.X1());
  const auto y1 = std::min(a.Y1(), b.Y1());
  return x1 > x0 && y1 > y0 ? RectI{x0, y0, x1 - x0, y1 - y0} : RectI{};
}

auto BrushDabOutputTexelSupport(const BrushCanonicalSample& sample, Vector2 translation,
                                Extent2D raster, Extent2D full_reference) -> RectI {
  return DabSupport(sample, translation, raster, full_reference);
}

auto BrushDabLocalTexelSupport(const BrushCanonicalSample& sample, Extent2D raster,
                               Extent2D full_reference) -> RectI {
  return DabSupport(sample, {}, raster, full_reference);
}

auto BrushStrokeOutputTexelSupport(const BrushStroke& stroke, Vector2 translation, Extent2D raster,
                                   Extent2D full_reference) -> RectI {
  RectI support{};
  for (const auto& sample : BrushStrokeSamples(stroke)) {
    support = UnionTexelRect(
        support, DabSupport(sample, translation, raster, full_reference));
  }
  return support;
}

auto BrushSourceOutputTexelSupport(const BrushMaskSource& source, Extent2D raster,
                                   Extent2D full_reference) -> RectI {
  RectI support{};
  for (const auto& stroke : source.strokes) {
    support = UnionTexelRect(
        support, BrushStrokeOutputTexelSupport(stroke, source.placement_translation, raster,
                                               full_reference));
  }
  return support;
}

auto BrushOutputTexelsToLocal(RectI output_texels, Vector2 translation, Extent2D raster,
                              Extent2D full_reference) -> RectI {
  RequirePositiveExtent(raster, "raster");
  RequirePositiveExtent(full_reference, "full_reference");
  RequireFiniteTranslation(translation);
  const auto clipped = ClipRasterDirtyRectangle(output_texels, raster);
  if (RectIEmpty(clipped)) {
    return {};
  }
  const float inv_scale_x =
      static_cast<float>(full_reference.width) / static_cast<float>(raster.width);
  const float inv_scale_y =
      static_cast<float>(full_reference.height) / static_cast<float>(raster.height);
  const float ref_x0     = static_cast<float>(clipped.x) * inv_scale_x;
  const float ref_y0     = static_cast<float>(clipped.y) * inv_scale_y;
  const float ref_x1     = static_cast<float>(clipped.X1()) * inv_scale_x;
  const float ref_y1     = static_cast<float>(clipped.Y1()) * inv_scale_y;
  const float local_x0   = ref_x0 - translation.x;
  const float local_y0   = ref_y0 - translation.y;
  const float local_x1   = ref_x1 - translation.x;
  const float local_y1   = ref_y1 - translation.y;
  const auto scale_x =
      static_cast<float>(raster.width) / static_cast<float>(full_reference.width);
  const auto scale_y =
      static_cast<float>(raster.height) / static_cast<float>(full_reference.height);
  const auto  x0      = static_cast<std::int32_t>(std::floor(local_x0 * scale_x));
  const auto  y0      = static_cast<std::int32_t>(std::floor(local_y0 * scale_y));
  const auto  x1      = static_cast<std::int32_t>(std::ceil(local_x1 * scale_x));
  const auto  y1      = static_cast<std::int32_t>(std::ceil(local_y1 * scale_y));
  return ClipRasterDirtyRectangle({x0, y0, x1 - x0, y1 - y0}, raster);
}

auto EffectiveMaskTexelSupport(const MaskModel& mask, Extent2D raster, Extent2D full_reference)
    -> RectI {
  RequirePositiveExtent(raster, "raster");
  RequirePositiveExtent(full_reference, "full_reference");
  if (!mask.enabled) {
    return {};
  }
  if (mask.invert) {
    return FullTexelRect(raster);
  }
  if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
    if (brush->feather_radius > 0.0f) {
      return FullTexelRect(raster);
    }
    return BrushSourceOutputTexelSupport(*brush, raster, full_reference);
  }
  if (std::holds_alternative<LinearGradientMaskSource>(mask.source)) {
    return FullTexelRect(raster);
  }
  const auto& radial = std::get<RadialMaskSource>(mask.source);
  const float outer  = 1.0f + radial.outer_feather;
  const float c      = std::cos(radial.rotation);
  const float s      = std::sin(radial.rotation);
  const float x_ext  = outer * std::hypot(radial.major_radius * c, radial.minor_radius * s);
  const float y_ext  = outer * std::hypot(radial.major_radius * s, radial.minor_radius * c);
  const float nx0    = radial.center_x - x_ext;
  const float ny0    = radial.center_y - y_ext;
  const float nx1    = radial.center_x + x_ext;
  const float ny1    = radial.center_y + y_ext;
  const auto  x0 =
      static_cast<std::int32_t>(std::floor(nx0 * static_cast<float>(raster.width) - 0.5f));
  const auto y0 =
      static_cast<std::int32_t>(std::floor(ny0 * static_cast<float>(raster.height) - 0.5f));
  const auto x1 =
      static_cast<std::int32_t>(std::floor(nx1 * static_cast<float>(raster.width) - 0.5f)) + 1;
  const auto y1 =
      static_cast<std::int32_t>(std::floor(ny1 * static_cast<float>(raster.height) - 0.5f)) + 1;
  return ClipRasterDirtyRectangle({x0, y0, x1 - x0, y1 - y0}, raster);
}

}  // namespace alcedo
