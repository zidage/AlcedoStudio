//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/grade_mask_coverage.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"

namespace alcedo {
namespace {

constexpr float kAnalyticEpsilon = 1.0e-6f;

[[noreturn]] void FailCoverage(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

auto AnyEnabled(std::span<const MaskModel> masks) -> bool {
  return std::any_of(masks.begin(), masks.end(),
                     [](const MaskModel& mask) { return mask.enabled; });
}

auto NeedsFullFeather(const MaskModel& mask) -> bool {
  const auto* brush = std::get_if<BrushMaskSource>(&mask.source);
  return brush != nullptr && mask.enabled && brush->feather_radius > 0.0f;
}

auto ApplyInvertOpacity(float coverage, bool invert, float opacity) -> std::uint8_t {
  if (invert) {
    coverage = 1.0f - coverage;
  }
  return QuantizeMaskCoverageToR8(std::clamp(coverage * opacity, 0.0f, 1.0f));
}

void UnionMaxInto(std::span<std::uint8_t> mix, std::span<const std::uint8_t> scratch, RectI region,
                  Extent2D raster) {
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto index =
          PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster);
      mix[index] = mix[index] > scratch[index] ? mix[index] : scratch[index];
    }
  }
}

auto CopyRegion(std::span<const std::uint8_t> pixels, RectI region, Extent2D raster)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(region.width) *
                                  static_cast<std::size_t>(region.height));
  std::size_t i = 0;
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto px = static_cast<std::uint32_t>(x);
      const auto py = static_cast<std::uint32_t>(y);
      bytes[i++]    = pixels[PackedR8Index(px, py, raster)];
    }
  }
  return bytes;
}

void RestoreRegion(std::span<std::uint8_t> pixels, RectI region, Extent2D raster,
                   std::span<const std::uint8_t> bytes) {
  std::size_t i = 0;
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      pixels[PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster)] =
          bytes[i++];
    }
  }
}

}  // namespace

void GradeMaskCoverage::RequireGeometry() const {
  if (raster_.Empty() || mix_.size() != static_cast<std::size_t>(raster_.width) * raster_.height) {
    FailCoverage("grade mask coverage geometry is unset");
  }
}

void GradeMaskCoverage::FillMix(std::uint8_t value) {
  std::fill(mix_.begin(), mix_.end(), value);
}

void GradeMaskCoverage::SetGeometry(Extent2D raster, Extent2D full_reference,
                                    std::uint32_t tile_texels) {
  if (tile_texels == 0) {
    FailCoverage("spatial index tile size must be positive");
  }
  rasterizer_.SetGeometry(raster, full_reference);
  raster_          = raster;
  full_reference_  = full_reference;
  tile_texels_     = tile_texels;
  mix_.assign(static_cast<std::size_t>(raster.width) * raster.height, 0);
  scratch_.assign(mix_.size(), 0);
  brush_indices_.clear();
}

void GradeMaskCoverage::BindBrushSource(const MaskId& mask_id, const BrushMaskSource& source) {
  RequireGeometry();
  BrushSpatialIndex index(tile_texels_);
  index.Rebuild(source, raster_, full_reference_);
  brush_indices_.insert_or_assign(mask_id, std::move(index));
}

void GradeMaskCoverage::UnbindMask(const MaskId& mask_id) { brush_indices_.erase(mask_id); }

auto GradeMaskCoverage::BrushIndex(const MaskId& mask_id) const -> const BrushSpatialIndex* {
  const auto found = brush_indices_.find(mask_id);
  return found == brush_indices_.end() ? nullptr : &found->second;
}

auto GradeMaskCoverage::AnalyticCoverage(const MaskModel& mask, std::uint32_t x,
                                         std::uint32_t y) const -> float {
  const auto center =
      CanonicalBrushTexelReferenceCenter(x, y, raster_, full_reference_);
  const float nx = center.x / static_cast<float>(full_reference_.width);
  const float ny = center.y / static_cast<float>(full_reference_.height);
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const float c      = std::cos(radial->rotation);
    const float s      = std::sin(radial->rotation);
    const float dx     = nx - radial->center_x;
    const float dy     = ny - radial->center_y;
    const float rx     = (c * dx + s * dy) / std::max(radial->major_radius, kAnalyticEpsilon);
    const float ry     = (-s * dx + c * dy) / std::max(radial->minor_radius, kAnalyticEpsilon);
    const float rho    = std::sqrt(rx * rx + ry * ry);
    const float inner  = std::max(0.0f, 1.0f - radial->inner_feather);
    const float outer  = 1.0f + radial->outer_feather;
    return 1.0f - std::clamp((rho - inner) / std::max(outer - inner, kAnalyticEpsilon), 0.0f, 1.0f);
  }
  const auto& linear        = std::get<LinearGradientMaskSource>(mask.source);
  const float normal_length = std::hypot(linear.normal_x, linear.normal_y);
  const float normal_x      = linear.normal_x / std::max(normal_length, kAnalyticEpsilon);
  const float normal_y      = linear.normal_y / std::max(normal_length, kAnalyticEpsilon);
  const float distance      = (nx - linear.origin_x) * normal_x + (ny - linear.origin_y) * normal_y;
  const float t =
      std::clamp(distance / std::max(linear.transition_distance, kAnalyticEpsilon) + 0.5f, 0.0f,
                 1.0f);
  return linear.start_value + (linear.end_value - linear.start_value) * t;
}

void GradeMaskCoverage::WriteEffectiveAnalytic(const MaskModel& mask, RectI region,
                                               std::span<std::uint8_t> dest) {
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto index =
          PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_);
      dest[index] = ApplyInvertOpacity(
          AnalyticCoverage(mask, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)),
          mask.invert, mask.opacity);
    }
  }
}

void GradeMaskCoverage::WriteEffectiveBrush(const MaskModel& mask, const BrushMaskSource& brush,
                                            RectI region, std::span<std::uint8_t> dest) {
  const auto* index = BrushIndex(mask.id);
  if (index == nullptr) {
    FailCoverage("brush spatial index is not bound for the Mask");
  }
  const bool feathered = brush.feather_radius > 0.0f;
  if (feathered) {
    rasterizer_.ClearToZero();
    rasterizer_.ReplayRegion(brush, *index, FullTexelRect(raster_));
    const auto radius_texels = BrushFeatherRadiusToSourceTexels(
        brush.feather_radius, raster_, CanonicalBrushReferenceBounds(), full_reference_);
    const auto feathered_pixels =
        feather_.Apply(rasterizer_.Pixels(), raster_, radius_texels, mask.invert, mask.opacity);
    for (std::int32_t y = region.y; y < region.Y1(); ++y) {
      for (std::int32_t x = region.x; x < region.X1(); ++x) {
        const auto i =
            PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_);
        dest[i] = feathered_pixels[i];
      }
    }
    return;
  }
  rasterizer_.ReplayRegion(brush, *index, region);
  const auto source = rasterizer_.Pixels();
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto i =
          PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_);
      dest[i] = ApplyInvertOpacity(CoverageFromMaskR8(source[i]), mask.invert, mask.opacity);
    }
  }
}

void GradeMaskCoverage::ReplayClippedRegion(std::span<const MaskModel> masks, RectI dirty) {
  const auto previous = CopyRegion(mix_, dirty, raster_);
  try {
    for (std::int32_t y = dirty.y; y < dirty.Y1(); ++y) {
      for (std::int32_t x = dirty.x; x < dirty.X1(); ++x) {
        mix_[PackedR8Index(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_)] =
            0;
      }
    }
    for (const auto& mask : masks) {
      if (!mask.enabled) {
        continue;
      }
      std::fill(scratch_.begin(), scratch_.end(), 0);
      if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
        WriteEffectiveBrush(mask, *brush, dirty, scratch_);
      } else {
        WriteEffectiveAnalytic(mask, dirty, scratch_);
      }
      UnionMaxInto(mix_, scratch_, dirty, raster_);
    }
  } catch (...) {
    RestoreRegion(mix_, dirty, raster_, previous);
    throw;
  }
}

void GradeMaskCoverage::EvaluateFull(std::span<const MaskModel> masks) {
  RequireGeometry();
  ReplayRegion(masks, FullTexelRect(raster_));
}

void GradeMaskCoverage::ReplayRegion(std::span<const MaskModel> masks, RectI dirty) {
  RequireGeometry();
  if (masks.empty()) {
    FillMix(255);
    return;
  }
  if (!AnyEnabled(masks)) {
    FillMix(0);
    return;
  }
  auto region = ClipTexelRect(dirty, raster_);
  for (const auto& mask : masks) {
    if (NeedsFullFeather(mask)) {
      region = FullTexelRect(raster_);
      break;
    }
  }
  if (RectIEmpty(region)) {
    return;
  }
  ReplayClippedRegion(masks, region);
}

}  // namespace alcedo
