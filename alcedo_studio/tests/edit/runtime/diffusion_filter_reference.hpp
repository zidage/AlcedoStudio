//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// CPU reference and fixtures shared by the CUDA and OpenCL diffusion filter tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"

namespace alcedo::diffusion_filter_test {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

inline auto operator+(Rgb lhs, Rgb rhs) -> Rgb {
  return {lhs.r + rhs.r, lhs.g + rhs.g, lhs.b + rhs.b};
}
inline auto operator*(Rgb lhs, double s) -> Rgb { return {lhs.r * s, lhs.g * s, lhs.b * s}; }

inline auto AcesccEncode(double value) -> float {
  constexpr double kA = 9.72;
  constexpr double kB = 17.52;
  if (value < 0.000030517578125) {
    return static_cast<float>((std::log2(0.0000152587890625 + value * 0.5) + kA) / kB);
  }
  return static_cast<float>((std::log2(value) + kA) / kB);
}

inline auto AcesccDecode(float value) -> double {
  constexpr double kA         = 9.72;
  constexpr double kB         = 17.52;
  constexpr double kFloor     = (-16.0 + kA) / kB;
  constexpr double kThreshold = (-15.0 + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (std::exp2(value * kB - kA) - 0.0000152587890625) * 2.0;
  return std::exp2(value * kB - kA);
}

inline auto Apply(const Matrix3x3& matrix, double x, double y) -> std::pair<double, double> {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

/// Double-precision reference of the scatter kernels (CUDA and OpenCL) on one linear render.
class ScatterReference {
 public:
  ScatterReference(std::vector<Rgb> linear, std::uint32_t width, std::uint32_t height,
                   const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping)
      : linear_(std::move(linear)),
        width_(width),
        height_(height),
        layout_(layout),
        mapping_(mapping) {}

  auto Mix() const -> std::vector<Rgb> {
    const auto       scatter = Scatter();
    const auto       base    = layout_.extents[0];
    std::vector<Rgb> out(linear_.size());
    for (std::uint32_t y = 0; y < height_; ++y) {
      for (std::uint32_t x = 0; x < width_; ++x) {
        const auto [bx, by] = Apply(mapping_.render_to_base, x + 0.5, y + 0.5);
        const auto glow     = SampleBSpline(scatter, base, bx, by);
        const auto direct   = linear_[static_cast<std::size_t>(y) * width_ + x];
        out[static_cast<std::size_t>(y) * width_ + x] =
            (direct * (1.0 - layout_.scatter_fraction) + glow * layout_.scatter_fraction) *
            layout_.transmission;
      }
    }
    return out;
  }

  auto Boost(Rgb value) const -> Rgb {
    value             = {std::max(value.r, 0.0), std::max(value.g, 0.0), std::max(value.b, 0.0)};
    const double peak = std::max(value.r, std::max(value.g, value.b));
    const double knee = layout_.highlight_knee;
    const double t    = std::clamp((peak - knee) / (1.0 - knee), 0.0, 1.0);
    return value * (1.0 + layout_.highlight_gain * t * t * (3.0 - 2.0 * t));
  }

 private:
  using Level = std::vector<Rgb>;

  static auto Fetch(const Level& level, ImageExtent extent, int x, int y) -> Rgb {
    x = std::clamp(x, 0, static_cast<int>(extent.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(extent.height) - 1);
    return level[static_cast<std::size_t>(y) * extent.width + x];
  }

  static auto Bilinear(const Level& level, ImageExtent extent, double px, double py) -> Rgb {
    const double fx = px - 0.5;
    const double fy = py - 0.5;
    const double x0 = std::floor(fx);
    const double y0 = std::floor(fy);
    const double ax = fx - x0;
    const double ay = fy - y0;
    const int    ix = static_cast<int>(x0);
    const int    iy = static_cast<int>(y0);
    const auto   top =
        Fetch(level, extent, ix, iy) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy) * ax;
    const auto bottom =
        Fetch(level, extent, ix, iy + 1) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy + 1) * ax;
    return top * (1.0 - ay) + bottom * ay;
  }

  static auto SampleBSpline(const Level& level, ImageExtent extent, double px, double py) -> Rgb {
    const double fx      = px - 0.5;
    const double fy      = py - 0.5;
    const double x0      = std::floor(fx);
    const double y0      = std::floor(fy);
    auto         weights = [](double t, double w[4]) {
      const double u = 1.0 - t;
      w[0]           = u * u * u / 6.0;
      w[1]           = (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0;
      w[2]           = (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0;
      w[3]           = t * t * t / 6.0;
    };
    double wx[4];
    double wy[4];
    weights(fx - x0, wx);
    weights(fy - y0, wy);
    Rgb sum;
    for (int j = 0; j < 4; ++j) {
      for (int i = 0; i < 4; ++i) {
        sum =
            sum + Fetch(level, extent, static_cast<int>(x0) - 1 + i, static_cast<int>(y0) - 1 + j) *
                      (wx[i] * wy[j]);
      }
    }
    return sum;
  }

  auto Scatter() const -> Level {
    const auto         count = layout_.level_count;
    std::vector<Level> levels(count);
    const auto         base    = layout_.extents[0];
    const ImageExtent  render  = {width_, height_};
    const auto         samples = mapping_.reduce_samples;
    const double       step    = 1.0 / samples;
    levels[0].resize(static_cast<std::size_t>(base.width) * base.height);
    for (std::uint32_t y = 0; y < base.height; ++y) {
      for (std::uint32_t x = 0; x < base.width; ++x) {
        Rgb sum;
        for (std::uint32_t j = 0; j < samples; ++j) {
          for (std::uint32_t i = 0; i < samples; ++i) {
            const auto [rx, ry] =
                Apply(mapping_.base_to_render, x + (i + 0.5) * step, y + (j + 0.5) * step);
            sum = sum + Boost(Bilinear(linear_, render, rx, ry));
          }
        }
        levels[0][static_cast<std::size_t>(y) * base.width + x] = sum * (step * step);
      }
    }
    for (std::uint32_t index = 1; index < count; ++index) {
      const auto from = layout_.extents[index - 1];
      const auto to   = layout_.extents[index];
      levels[index].resize(static_cast<std::size_t>(to.width) * to.height);
      for (std::uint32_t y = 0; y < to.height; ++y) {
        for (std::uint32_t x = 0; x < to.width; ++x) {
          const double cx = 2.0 * (x + 0.5);
          const double cy = 2.0 * (y + 0.5);
          auto         s  = [&](double ox, double oy) {
            return Bilinear(levels[index - 1], from, cx + ox, cy + oy);
          };
          const auto corners = s(-2, -2) + s(2, -2) + s(-2, 2) + s(2, 2);
          const auto edges   = s(0, -2) + s(-2, 0) + s(2, 0) + s(0, 2);
          const auto inner   = s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1);
          levels[index][static_cast<std::size_t>(y) * to.width + x] =
              s(0, 0) * 0.125 + corners * 0.03125 + edges * 0.0625 + inner * 0.125;
        }
      }
    }
    Level  coarse        = levels[count - 1];
    double coarse_weight = layout_.weights[count - 1];
    for (std::uint32_t index = count - 1; index-- > 0;) {
      const auto coarse_extent = layout_.extents[index + 1];
      const auto extent        = layout_.extents[index];
      Level      accumulated(static_cast<std::size_t>(extent.width) * extent.height);
      for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
          const double cx = (x + 0.5) * 0.5;
          const double cy = (y + 0.5) * 0.5;
          auto         s  = [&](double ox, double oy) {
            return Bilinear(coarse, coarse_extent, cx + ox, cy + oy);
          };
          const auto edges   = s(-1, 0) + s(1, 0) + s(0, -1) + s(0, 1);
          const auto corners = s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1);
          const auto tent    = (s(0, 0) * 4.0 + edges * 2.0 + corners) * (1.0 / 16.0);
          const auto i       = static_cast<std::size_t>(y) * extent.width + x;
          accumulated[i]     = levels[index][i] * layout_.weights[index] + tent * coarse_weight;
        }
      }
      coarse        = std::move(accumulated);
      coarse_weight = 1.0;
    }
    return coarse;
  }

  std::vector<Rgb>        linear_;
  std::uint32_t           width_;
  std::uint32_t           height_;
  DiffusionFilterLayout   layout_;
  DiffusionScatterMapping mapping_;
};

inline auto PointLightField(std::uint32_t width, std::uint32_t height, std::uint32_t light_x,
                            std::uint32_t light_y) -> std::vector<Rgb> {
  std::vector<Rgb> field(static_cast<std::size_t>(width) * height, Rgb{0.02, 0.018, 0.015});
  field[static_cast<std::size_t>(light_y) * width + light_x] = Rgb{40.0, 32.0, 24.0};
  return field;
}

/// Dark field with a bright @p size x @p size patch whose top-left corner is (@p x, @p y).
inline auto PatchField(std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y,
                       std::uint32_t size) -> std::vector<Rgb> {
  std::vector<Rgb> field(static_cast<std::size_t>(width) * height, Rgb{0.02, 0.018, 0.015});
  for (std::uint32_t row = y; row < y + size; ++row) {
    for (std::uint32_t col = x; col < x + size; ++col) {
      field[static_cast<std::size_t>(row) * width + col] = Rgb{40.0, 32.0, 24.0};
    }
  }
  return field;
}

inline void ExpectMatchesReference(const std::vector<Rgba>& pixels,
                                   const std::vector<Rgb>&  expected) {
  ASSERT_EQ(pixels.size(), expected.size());
  // The GPU reads float32 ACEScc and sums in float32.
  auto tolerance = [](double value) { return 2.0e-4 * (1.0 + std::fabs(value)); };
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    ASSERT_NEAR(pixels[i].r, expected[i].r, tolerance(expected[i].r)) << "pixel " << i;
    ASSERT_NEAR(pixels[i].g, expected[i].g, tolerance(expected[i].g)) << "pixel " << i;
    ASSERT_NEAR(pixels[i].b, expected[i].b, tolerance(expected[i].b)) << "pixel " << i;
    ASSERT_EQ(pixels[i].a, 0.75f);
  }
}

}  // namespace alcedo::diffusion_filter_test
