//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/brush_signed_distance.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"

namespace alcedo {
namespace {

constexpr float kSignedDistanceInfinite = 1.0e20f;

[[noreturn]] void FailFeather(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

auto FinishEffective(float value, bool invert, float opacity) -> std::uint8_t {
  if (invert) {
    value = 1.0f - value;
  }
  return QuantizeMaskCoverageToR8(std::clamp(value * opacity, 0.0f, 1.0f));
}

}  // namespace

void BrushSignedDistanceFeather::EnsureScratch(std::uint32_t width, std::uint32_t height) {
  const auto count = static_cast<std::size_t>(width) * height;
  width_           = width;
  height_          = height;
  horizontal_.assign(count, kSignedDistanceInfinite);
  inside_.assign(count, kSignedDistanceInfinite);
  outside_.assign(count, kSignedDistanceInfinite);
  sites_.assign(count, 0);
  boundaries_.assign(count, 0.0f);
}

void BrushSignedDistanceFeather::BandHorizontal(std::span<const std::uint8_t> source,
                                                bool want_inside) {
  auto& destination = want_inside ? inside_ : outside_;
  for (std::uint32_t y = 0; y < height_; ++y) {
    int nearest = -1;
    for (std::uint32_t x = 0; x < width_; ++x) {
      const auto  index  = PackedR8Index(x, y, {width_, height_});
      const bool  inside = source[index] >= 128;
      if (inside == want_inside) {
        nearest = static_cast<int>(x);
      }
      const float dx          = static_cast<float>(static_cast<int>(x) - nearest);
      horizontal_[index]      = nearest < 0 ? kSignedDistanceInfinite : dx * dx;
    }
    nearest = static_cast<int>(width_);
    for (int x = static_cast<int>(width_) - 1; x >= 0; --x) {
      const auto  index  = PackedR8Index(static_cast<std::uint32_t>(x), y, {width_, height_});
      const bool  inside = source[index] >= 128;
      if (inside == want_inside) {
        nearest = x;
      }
      if (nearest < static_cast<int>(width_)) {
        const float dx     = static_cast<float>(x - nearest);
        horizontal_[index] = std::min(horizontal_[index], dx * dx);
      }
    }
  }
  BandVertical(destination);
}

void BrushSignedDistanceFeather::BandVertical(std::span<float> squared_distance) {
  for (std::uint32_t x = 0; x < width_; ++x) {
    const auto site_base     = static_cast<std::size_t>(x) * height_;
    const auto boundary_base = site_base;
    int        count         = 0;
    for (std::uint32_t y = 0; y < height_; ++y) {
      const float f = horizontal_[PackedR8Index(x, y, {width_, height_})];
      if (f >= 1.0e19f) {
        continue;
      }
      float boundary = -1.0e20f;
      while (count > 0) {
        const int   previous   = sites_[site_base + static_cast<std::size_t>(count - 1)];
        const float previous_f =
            horizontal_[PackedR8Index(x, static_cast<std::uint32_t>(previous), {width_, height_})];
        boundary = ((f + static_cast<float>(y * y)) -
                    (previous_f + static_cast<float>(previous * previous))) /
                   (2.0f * (static_cast<float>(y) - static_cast<float>(previous)));
        if (boundary > boundaries_[boundary_base + static_cast<std::size_t>(count - 1)]) {
          break;
        }
        --count;
      }
      sites_[site_base + static_cast<std::size_t>(count)] = static_cast<int>(y);
      boundaries_[boundary_base + static_cast<std::size_t>(count)] =
          count == 0 ? -1.0e20f : boundary;
      ++count;
    }
    if (count == 0) {
      for (std::uint32_t y = 0; y < height_; ++y) {
        squared_distance[PackedR8Index(x, y, {width_, height_})] = kSignedDistanceInfinite;
      }
      continue;
    }
    int site_index = 0;
    for (std::uint32_t y = 0; y < height_; ++y) {
      while (site_index + 1 < count &&
             boundaries_[boundary_base + static_cast<std::size_t>(site_index + 1)] <
                 static_cast<float>(y)) {
        ++site_index;
      }
      const int   site = sites_[site_base + static_cast<std::size_t>(site_index)];
      const float dy   = static_cast<float>(static_cast<int>(y) - site);
      squared_distance[PackedR8Index(x, y, {width_, height_})] =
          horizontal_[PackedR8Index(x, static_cast<std::uint32_t>(site), {width_, height_})] +
          dy * dy;
    }
  }
}

auto BrushSignedDistanceFeather::Apply(std::span<const std::uint8_t> source, Extent2D extent,
                                       float radius_texels, bool invert, float opacity)
    -> std::vector<std::uint8_t> {
  if (extent.Empty()) {
    FailFeather("signed-distance feather requires a positive extent");
  }
  if (source.size() != static_cast<std::size_t>(extent.width) * extent.height) {
    FailFeather("signed-distance source must be tightly packed R8");
  }
  if (!std::isfinite(radius_texels) || radius_texels < 0.0f) {
    FailFeather("feather radius_texels must be finite and nonnegative");
  }
  if (!std::isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
    FailFeather("opacity must stay in [0, 1]");
  }
  EnsureScratch(extent.width, extent.height);
  BandHorizontal(source, true);
  BandHorizontal(source, false);
  std::vector<std::uint8_t> output(source.size());
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = 0; x < width_; ++x) {
      const auto  index    = PackedR8Index(x, y, extent);
      const float coverage = CoverageFromMaskR8(source[index]);
      const bool  inside   = coverage >= 0.5f;
      float       signed_distance;
      if (coverage > 0.0f && coverage < 1.0f) {
        signed_distance = coverage - 0.5f;
      } else {
        const float exact = std::sqrt(inside ? outside_[index] : inside_[index]);
        const float to_boundary = std::max(exact - 0.5f, 0.0f);
        signed_distance         = inside ? to_boundary : -to_boundary;
      }
      float value = radius_texels <= 0.0f
                        ? (signed_distance >= 0.0f ? 1.0f : 0.0f)
                        : std::clamp(0.5f + signed_distance / (2.0f * radius_texels), 0.0f, 1.0f);
      value       = value * value * (3.0f - 2.0f * value);
      output[index] = FinishEffective(value, invert, opacity);
    }
  }
  return output;
}

}  // namespace alcedo
