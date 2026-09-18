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

namespace alcedo {
namespace {

constexpr float kAnalyticEpsilon = 1.0e-6f;

[[noreturn]] void FailCoverage(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

[[nodiscard]] auto RectEmpty(RectI rect) -> bool {
  return rect.width <= 0 || rect.height <= 0;
}

[[nodiscard]] auto PackedIndex(std::uint32_t x, std::uint32_t y, Extent2D extent) -> std::size_t {
  if (extent.Empty() || x >= extent.width || y >= extent.height) {
    FailCoverage("R8 index is outside the raster");
  }
  return static_cast<std::size_t>(y) * extent.width + x;
}

[[nodiscard]] auto FullRect(Extent2D extent) -> RectI {
  if (extent.Empty()) {
    return {};
  }
  return RectI{0, 0, static_cast<std::int32_t>(extent.width),
               static_cast<std::int32_t>(extent.height)};
}

[[nodiscard]] auto ClipRect(RectI rect, Extent2D extent) -> RectI {
  const std::int32_t x0 = std::max<std::int32_t>(rect.x, 0);
  const std::int32_t y0 = std::max<std::int32_t>(rect.y, 0);
  const std::int32_t x1 = std::min<std::int32_t>(rect.X1(), static_cast<std::int32_t>(extent.width));
  const std::int32_t y1 = std::min<std::int32_t>(rect.Y1(), static_cast<std::int32_t>(extent.height));
  return RectI{x0, y0, std::max<std::int32_t>(0, x1 - x0), std::max<std::int32_t>(0, y1 - y0)};
}

/// Reference-pixel center of raster texel `@p x, @p y`: `(x + 0.5) * full / raster`.
[[nodiscard]] auto TexelReferenceCenter(std::uint32_t x, std::uint32_t y, Extent2D raster,
                                        Extent2D full_reference) -> Vector2 {
  return Vector2{(static_cast<float>(x) + 0.5f) * static_cast<float>(full_reference.width) /
                     static_cast<float>(raster.width),
                 (static_cast<float>(y) + 0.5f) * static_cast<float>(full_reference.height) /
                     static_cast<float>(raster.height)};
}

/// Quantize coverage in `[0, 1]` to packed R8 with round-half-up.
[[nodiscard]] auto QuantizeToR8(float coverage) -> std::uint8_t {
  return static_cast<std::uint8_t>(
      std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
}

auto AnyEnabled(std::span<const MaskModel> masks) -> bool {
  return std::any_of(masks.begin(), masks.end(),
                     [](const MaskModel& mask) { return mask.enabled; });
}

auto ApplyInvertOpacity(float coverage, bool invert, float opacity) -> std::uint8_t {
  if (invert) {
    coverage = 1.0f - coverage;
  }
  return QuantizeToR8(std::clamp(coverage * opacity, 0.0f, 1.0f));
}

void UnionMaxInto(std::span<std::uint8_t> mix, std::span<const std::uint8_t> scratch, RectI region,
                  Extent2D raster) {
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      const auto index =
          PackedIndex(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster);
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
      bytes[i++]    = pixels[PackedIndex(px, py, raster)];
    }
  }
  return bytes;
}

void RestoreRegion(std::span<std::uint8_t> pixels, RectI region, Extent2D raster,
                   std::span<const std::uint8_t> bytes) {
  std::size_t i = 0;
  for (std::int32_t y = region.y; y < region.Y1(); ++y) {
    for (std::int32_t x = region.x; x < region.X1(); ++x) {
      pixels[PackedIndex(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster)] =
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

void GradeMaskCoverage::SetGeometry(Extent2D raster, Extent2D full_reference) {
  if (raster.Empty() || full_reference.Empty()) {
    FailCoverage("grade mask coverage requires a nonempty raster and full reference");
  }
  raster_         = raster;
  full_reference_ = full_reference;
  mix_.assign(static_cast<std::size_t>(raster.width) * raster.height, 0);
  scratch_.assign(mix_.size(), 0);
}

auto GradeMaskCoverage::AnalyticCoverage(const MaskModel& mask, std::uint32_t x,
                                         std::uint32_t y) const -> float {
  const auto center = TexelReferenceCenter(x, y, raster_, full_reference_);
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
          PackedIndex(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_);
      dest[index] = ApplyInvertOpacity(
          AnalyticCoverage(mask, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)),
          mask.invert, mask.opacity);
    }
  }
}

void GradeMaskCoverage::ReplayClippedRegion(std::span<const MaskModel> masks, RectI dirty) {
  const auto previous = CopyRegion(mix_, dirty, raster_);
  try {
    for (std::int32_t y = dirty.y; y < dirty.Y1(); ++y) {
      for (std::int32_t x = dirty.x; x < dirty.X1(); ++x) {
        mix_[PackedIndex(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), raster_)] =
            0;
      }
    }
    for (const auto& mask : masks) {
      if (!mask.enabled) {
        continue;
      }
      std::fill(scratch_.begin(), scratch_.end(), 0);
      WriteEffectiveAnalytic(mask, dirty, scratch_);
      UnionMaxInto(mix_, scratch_, dirty, raster_);
    }
  } catch (...) {
    RestoreRegion(mix_, dirty, raster_, previous);
    throw;
  }
}

void GradeMaskCoverage::EvaluateFull(std::span<const MaskModel> masks) {
  RequireGeometry();
  ReplayRegion(masks, FullRect(raster_));
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
  const auto region = ClipRect(dirty, raster_);
  if (RectEmpty(region)) {
    return;
  }
  ReplayClippedRegion(masks, region);
}

}  // namespace alcedo
