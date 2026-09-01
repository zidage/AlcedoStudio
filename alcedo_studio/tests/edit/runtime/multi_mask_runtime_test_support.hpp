//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"

namespace alcedo::multi_mask_test {

/** @brief R8 comparisons allow at most one code value. */
inline constexpr int kR8ToleranceCodes = 1;

/**
 * @brief Round-half-up R8 encoding used by CUDA, OpenCL, and Metal coverage writes.
 *
 * `code = clamp(value * 255 + 0.5, 0, 255)` as uint8.
 */
inline auto QuantizeR8(float value) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
}

/**
 * @brief Invert, then opacity, then clamp. Range fields are identity (1) in this runtime.
 */
inline auto ApplyInvertAndOpacity(float coverage, bool invert, float opacity) -> float {
  if (invert) {
    coverage = 1.0f - coverage;
  }
  return std::clamp(coverage * opacity, 0.0f, 1.0f);
}

inline auto Transform(const Matrix3x3& matrix, float x, float y) -> Vector2 {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

inline auto SampleR8Bilinear(std::span<const std::uint8_t> pixels, std::uint32_t width,
                             std::uint32_t height, float u, float v) -> float {
  if (pixels.empty() || width == 0 || height == 0 || u < 0.0f || v < 0.0f || u > 1.0f ||
      v > 1.0f) {
    return 0.0f;
  }
  const float x  = u * static_cast<float>(width) - 0.5f;
  const float y  = v * static_cast<float>(height) - 0.5f;
  const int   x0 = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(width) - 1);
  const int   y0 = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(height) - 1);
  const int   x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = std::clamp(x - std::floor(x), 0.0f, 1.0f);
  const float ty = std::clamp(y - std::floor(y), 0.0f, 1.0f);
  const auto  at = [&](int px, int py) -> float {
    return static_cast<float>(pixels[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)]);
  };
  const float a = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
  const float b = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
  return (a * (1.0f - ty) + b * ty) / 255.0f;
}

inline auto RadialSourceCoverage(const RadialMaskSource& radial, float nx, float ny) -> float {
  const float c      = std::cos(radial.rotation);
  const float s      = std::sin(radial.rotation);
  const float dx     = nx - radial.center_x;
  const float dy     = ny - radial.center_y;
  const float rx     = (c * dx + s * dy) / std::max(radial.major_radius, 1.0e-6f);
  const float ry     = (-s * dx + c * dy) / std::max(radial.minor_radius, 1.0e-6f);
  const float radius = std::sqrt(rx * rx + ry * ry);
  const float inner  = std::max(0.0f, 1.0f - radial.inner_feather);
  const float outer  = 1.0f + radial.outer_feather;
  return 1.0f - std::clamp((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f, 1.0f);
}

inline auto LinearGradientSourceCoverage(const LinearGradientMaskSource& gradient, float nx,
                                         float ny) -> float {
  const float length   = std::hypot(gradient.normal_x, gradient.normal_y);
  const float normal_x = gradient.normal_x / std::max(length, 1.0e-6f);
  const float normal_y = gradient.normal_y / std::max(length, 1.0e-6f);
  const float distance =
      (nx - gradient.origin_x) * normal_x + (ny - gradient.origin_y) * normal_y;
  const float t =
      std::clamp(distance / std::max(gradient.transition_distance, 1.0e-6f) + 0.5f, 0.0f, 1.0f);
  return gradient.start_value + (gradient.end_value - gradient.start_value) * t;
}

/** @brief Settled or active Brush pixels for one Mask. Empty pixels mean no-asset zero coverage. */
struct BrushRasterView {
  MaskId                        id;
  std::span<const std::uint8_t> pixels;
  Extent2D                      extent{};
};

inline auto FindBrushRaster(std::span<const BrushRasterView> rasters, const MaskId& id)
    -> const BrushRasterView* {
  for (const auto& raster : rasters) {
    if (raster.id == id) {
      return &raster;
    }
  }
  return nullptr;
}

inline auto NormalizedReference(const ResolvedRenderGeometry& geometry, std::uint32_t x,
                                std::uint32_t y) -> Vector2 {
  const auto reference =
      Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
  return {reference.x / static_cast<float>(geometry.full_reference_extent.width),
          reference.y / static_cast<float>(geometry.full_reference_extent.height)};
}

/**
 * @brief Effective coverage of one Mask at a render pixel. Disabled Masks are 0.
 *
 * Brush feather is not evaluated here. Union-matrix cases use zero-radius Brush
 * sources; signed-distance feather stays on the native path.
 */
inline auto EffectiveCoverageAt(const MaskModel& mask, std::span<const BrushRasterView> rasters,
                                const ResolvedRenderGeometry& geometry, std::uint32_t x,
                                std::uint32_t y) -> float {
  if (!mask.enabled) {
    return 0.0f;
  }
  float source = 0.0f;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const auto n = NormalizedReference(geometry, x, y);
    source       = RadialSourceCoverage(*radial, n.x, n.y);
  } else if (const auto* gradient = std::get_if<LinearGradientMaskSource>(&mask.source)) {
    const auto n = NormalizedReference(geometry, x, y);
    source       = LinearGradientSourceCoverage(*gradient, n.x, n.y);
  } else if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
    const auto* raster = FindBrushRaster(rasters, mask.id);
    if (raster == nullptr || raster->pixels.empty() || raster->extent.Empty()) {
      source = 0.0f;
    } else {
      const auto sampling = MakeRasterMaskSamplingPlan(geometry, brush->descriptor.reference_bounds,
                                                       raster->extent);
      const auto uv =
          Transform(sampling.render_to_texture_uv, static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f);
      source = SampleR8Bilinear(raster->pixels, raster->extent.width, raster->extent.height, uv.x,
                                uv.y);
    }
  }
  return ApplyInvertAndOpacity(source, mask.invert, mask.opacity);
}

/**
 * @brief Maximum of enabled Masks. An empty list is not a Mask image (Grade coverage 1).
 *
 * A nonempty all-disabled list is zero coverage.
 */
inline auto EvaluateEnabledUnionR8(std::span<const MaskModel> masks,
                                   std::span<const BrushRasterView> rasters,
                                   const ResolvedRenderGeometry& geometry)
    -> std::vector<std::uint8_t> {
  const auto width  = geometry.render_extent.width;
  const auto height = geometry.render_extent.height;
  std::vector<std::uint8_t> output(static_cast<std::size_t>(width) * height, 0);
  bool                      any_enabled = false;
  for (const auto& mask : masks) {
    any_enabled = any_enabled || mask.enabled;
  }
  if (!any_enabled) {
    return output;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      float coverage = 0.0f;
      for (const auto& mask : masks) {
        coverage = std::max(coverage, EffectiveCoverageAt(mask, rasters, geometry, x, y));
      }
      output[static_cast<std::size_t>(y) * width + x] = QuantizeR8(coverage);
    }
  }
  return output;
}

inline void ExpectR8WithinTolerance(std::span<const std::uint8_t> actual,
                                    std::span<const std::uint8_t> expected,
                                    int tolerance = kR8ToleranceCodes) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_NEAR(actual[i], expected[i], tolerance) << "index " << i;
  }
}

/**
 * @brief After ResetCounters, the only R8 rectangle upload is @p expected and it is
 * not a full-raster transfer.
 *
 * Plan execution may also copy kernel parameters, so total host-to-device bytes can
 * exceed the rectangle size while remaining below a full R8 raster.
 */
inline void ExpectDirtyR8RectangleUpload(const std::vector<RectI>& rectangles, RectI expected,
                                         std::uint64_t host_to_device_bytes,
                                         std::uint32_t raster_width, std::uint32_t raster_height) {
  ASSERT_EQ(rectangles.size(), 1U);
  EXPECT_EQ(rectangles.front(), expected);
  const auto rect_bytes =
      static_cast<std::uint64_t>(expected.width) * static_cast<std::uint64_t>(expected.height);
  const auto full_r8 =
      static_cast<std::uint64_t>(raster_width) * static_cast<std::uint64_t>(raster_height);
  EXPECT_GE(host_to_device_bytes, rect_bytes);
  EXPECT_LT(host_to_device_bytes, full_r8);
}

/** @brief Host assets that keep @ref BrushRasterView spans alive. */
struct LoadedBrushRasters {
  std::vector<std::shared_ptr<const MaskAsset>> assets;
  std::vector<BrushRasterView>                  views;
};

inline auto LoadBrushRasters(MaskStore& store, std::span<const MaskModel> masks)
    -> LoadedBrushRasters {
  LoadedBrushRasters loaded;
  for (const auto& mask : masks) {
    const auto* brush = std::get_if<BrushMaskSource>(&mask.source);
    if (brush == nullptr || !brush->asset_key.has_value() || brush->asset_key->Empty()) {
      continue;
    }
    auto asset = store.Load(*brush->asset_key);
    loaded.views.push_back(BrushRasterView{mask.id, asset->pixels, asset->descriptor.extent});
    loaded.assets.push_back(std::move(asset));
  }
  return loaded;
}

}  // namespace alcedo::multi_mask_test
