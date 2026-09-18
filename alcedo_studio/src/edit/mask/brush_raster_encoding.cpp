//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_raster_encoding.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

constexpr float kBoundsScaleEpsilon = 1.0e-6f;

[[noreturn]] void Fail(std::string_view message) {
  throw std::invalid_argument(std::string{message});
}

void RequireFinite(float value, std::string_view name) {
  if (!std::isfinite(value)) {
    Fail(std::string{name} + " must be finite");
  }
}

void RequirePositiveExtent(Extent2D extent, std::string_view name) {
  if (extent.Empty()) {
    Fail(std::string{name} + " must be positive");
  }
}

}  // namespace

void ValidateBrushCanonicalSample(const BrushCanonicalSample& sample) {
  RequireFinite(sample.local_x, "local_x");
  RequireFinite(sample.local_y, "local_y");
  RequireFinite(sample.radius, "radius");
  RequireFinite(sample.strength, "strength");
  RequireFinite(sample.hardness, "hardness");
  if (!(sample.radius > 0.0f)) {
    Fail("radius must be positive");
  }
  if (sample.strength < 0.0f || sample.strength > 1.0f) {
    Fail("strength must stay in [0, 1]");
  }
  if (sample.hardness < 0.0f || sample.hardness > 1.0f) {
    Fail("hardness must stay in [0, 1]");
  }
}

auto QuantizeMaskCoverageToR8(float coverage) -> std::uint8_t {
  RequireFinite(coverage, "coverage");
  const float scaled = coverage * 255.0f + 0.5f;
  const float clamped = std::min(std::max(scaled, 0.0f), 255.0f);
  return static_cast<std::uint8_t>(clamped);
}

auto BrushDabCoverage(float distance, float radius, float strength, float hardness) -> float {
  RequireFinite(distance, "distance");
  if (distance < 0.0f) {
    Fail("distance must be nonnegative");
  }
  BrushCanonicalSample sample;
  sample.radius   = radius;
  sample.strength = strength;
  sample.hardness = hardness;
  ValidateBrushCanonicalSample(sample);
  if (hardness >= 1.0f) {
    return distance <= radius ? strength : 0.0f;
  }
  const float inner = hardness * radius;
  if (distance <= inner) {
    return strength;
  }
  if (distance >= radius) {
    return 0.0f;
  }
  const float span = radius - inner;
  const float t    = (distance - inner) / span;
  return strength * (1.0f - t);
}

auto CanonicalBrushRasterExtent(Extent2D full_reference) -> Extent2D {
  RequirePositiveExtent(full_reference, "full_reference");
  const auto long_edge = std::max(full_reference.width, full_reference.height);
  if (long_edge <= kMaximumRasterMaskAxis) {
    return full_reference;
  }
  const float scale = static_cast<float>(kMaximumRasterMaskAxis) / static_cast<float>(long_edge);
  const auto width = static_cast<std::uint32_t>(
      std::max(1.0f, std::ceil(static_cast<float>(full_reference.width) * scale)));
  const auto height = static_cast<std::uint32_t>(
      std::max(1.0f, std::ceil(static_cast<float>(full_reference.height) * scale)));
  return {std::min(width, kMaximumRasterMaskAxis), std::min(height, kMaximumRasterMaskAxis)};
}

auto CanonicalBrushTexelReferenceCenter(std::uint32_t x, std::uint32_t y, Extent2D raster,
                                        Extent2D full_reference) -> Vector2 {
  RequirePositiveExtent(raster, "raster");
  RequirePositiveExtent(full_reference, "full_reference");
  if (x >= raster.width || y >= raster.height) {
    Fail("texel is outside the canonical raster");
  }
  const auto center = PixelCenter(x, y);
  return {center.x * static_cast<float>(full_reference.width) / static_cast<float>(raster.width),
          center.y * static_cast<float>(full_reference.height) /
              static_cast<float>(raster.height)};
}

auto BrushFeatherRadiusToSourceTexels(float feather_radius, Extent2D raster, NormalizedRect bounds,
                                      Extent2D full_reference) -> float {
  RequireFinite(feather_radius, "feather_radius");
  if (feather_radius < 0.0f) {
    Fail("feather_radius must be nonnegative");
  }
  RequirePositiveExtent(raster, "raster");
  RequirePositiveExtent(full_reference, "full_reference");
  RequireFinite(bounds.x, "reference_bounds.x");
  RequireFinite(bounds.y, "reference_bounds.y");
  RequireFinite(bounds.w, "reference_bounds.w");
  RequireFinite(bounds.h, "reference_bounds.h");
  const float x_scale = static_cast<float>(raster.width) /
                        (static_cast<float>(full_reference.width) *
                         std::max(bounds.w, kBoundsScaleEpsilon));
  const float y_scale = static_cast<float>(raster.height) /
                        (static_cast<float>(full_reference.height) *
                         std::max(bounds.h, kBoundsScaleEpsilon));
  return feather_radius * 0.5f * (x_scale + y_scale);
}

auto SamplePackedR8Bilinear(std::span<const std::uint8_t> pixels, Extent2D extent, float u, float v)
    -> float {
  RequirePositiveExtent(extent, "extent");
  const auto required =
      static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height);
  if (pixels.size() != required) {
    Fail("pixels must be tightly packed R8");
  }
  if (!std::isfinite(u) || !std::isfinite(v) || u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return 0.0f;
  }
  const float x  = u * static_cast<float>(extent.width) - 0.5f;
  const float y  = v * static_cast<float>(extent.height) - 0.5f;
  const int   x0 = std::max(
      0, std::min(static_cast<int>(extent.width) - 1, static_cast<int>(std::floor(x))));
  const int y0 = std::max(
      0, std::min(static_cast<int>(extent.height) - 1, static_cast<int>(std::floor(y))));
  const int   x1 = std::min(x0 + 1, static_cast<int>(extent.width) - 1);
  const int   y1 = std::min(y0 + 1, static_cast<int>(extent.height) - 1);
  const float tx = std::min(std::max(x - std::floor(x), 0.0f), 1.0f);
  const float ty = std::min(std::max(y - std::floor(y), 0.0f), 1.0f);
  const float a  = static_cast<float>(pixels[static_cast<std::size_t>(y0) * extent.width +
                                            static_cast<std::size_t>(x0)]) *
                      (1.0f - tx) +
                  static_cast<float>(pixels[static_cast<std::size_t>(y0) * extent.width +
                                            static_cast<std::size_t>(x1)]) *
                      tx;
  const float b = static_cast<float>(pixels[static_cast<std::size_t>(y1) * extent.width +
                                            static_cast<std::size_t>(x0)]) *
                      (1.0f - tx) +
                  static_cast<float>(pixels[static_cast<std::size_t>(y1) * extent.width +
                                            static_cast<std::size_t>(x1)]) *
                      tx;
  return (a * (1.0f - ty) + b * ty) / 255.0f;
}

}  // namespace alcedo
