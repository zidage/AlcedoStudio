//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mask_thumbnail_evaluate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/geometry/types.hpp"

namespace alcedo {
namespace {

constexpr float kAnalyticEpsilon = 1.0e-6f;

[[nodiscard]] auto QuantizeToR8(float coverage) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
}

[[nodiscard]] auto ApplyInvertOpacity(float coverage, bool invert, float opacity) -> std::uint8_t {
  if (invert) {
    coverage = 1.0f - coverage;
  }
  return QuantizeToR8(std::clamp(coverage * opacity, 0.0f, 1.0f));
}

[[nodiscard]] auto RadialCoverage(const RadialMaskParams& radial, float nx, float ny) -> float {
  const float c     = std::cos(radial.rotation);
  const float s     = std::sin(radial.rotation);
  const float dx    = nx - radial.center_x;
  const float dy    = ny - radial.center_y;
  const float rx    = (c * dx + s * dy) / std::max(radial.major_radius, kAnalyticEpsilon);
  const float ry    = (-s * dx + c * dy) / std::max(radial.minor_radius, kAnalyticEpsilon);
  const float rho   = std::sqrt(rx * rx + ry * ry);
  const float inner = std::max(0.0f, 1.0f - radial.inner_feather);
  const float outer = 1.0f + radial.outer_feather;
  return 1.0f - std::clamp((rho - inner) / std::max(outer - inner, kAnalyticEpsilon), 0.0f, 1.0f);
}

[[nodiscard]] auto LinearCoverage(const LinearGradientMaskParams& linear, float nx, float ny)
    -> float {
  const float normal_length = std::hypot(linear.normal_x, linear.normal_y);
  const float normal_x      = linear.normal_x / std::max(normal_length, kAnalyticEpsilon);
  const float normal_y      = linear.normal_y / std::max(normal_length, kAnalyticEpsilon);
  const float distance      = (nx - linear.origin_x) * normal_x + (ny - linear.origin_y) * normal_y;
  const float t =
      std::clamp(distance / std::max(linear.transition_distance, kAnalyticEpsilon) + 0.5f, 0.0f,
                 1.0f);
  return linear.start_value + (linear.end_value - linear.start_value) * t;
}

[[nodiscard]] auto LayerCoverage(const MaskThumbnailLayer& layer, float nx, float ny) -> float {
  if (layer.kind == MaskSourceKind::Radial) {
    return RadialCoverage(std::get<RadialMaskParams>(layer.params), nx, ny);
  }
  return LinearCoverage(std::get<LinearGradientMaskParams>(layer.params), nx, ny);
}

}  // namespace

auto RenderMaskThumbnail(const MaskThumbnailSpec& spec) -> QImage {
  if (spec.geometry.full_reference.Empty()) {
    throw std::runtime_error("Mask thumbnail render requires a nonempty full reference");
  }
  ImageGeometryParams image;
  image.crop_rect        = spec.geometry.crop_rect;
  image.rotation_degrees = spec.geometry.rotation_degrees;
  image.expand_to_fit    = spec.geometry.expand_to_fit;
  ResolutionRequest resolution;
  resolution.render_scale = 1.0f;
  resolution.max_edge     = kMaskThumbnailSize;
  resolution.quality      = RenderQuality::Preview;
  const auto geometry     = ResolveRenderGeometry(
      MakeSourceGeometry(spec.geometry.full_reference, spec.geometry.full_reference), image,
      ViewRequest{}, resolution, SamplingFootprint{});
  if (geometry.render_extent.Empty() ||
      geometry.render_extent.width > kMaskThumbnailSize ||
      geometry.render_extent.height > kMaskThumbnailSize) {
    throw std::runtime_error("Mask thumbnail render extent is invalid");
  }

  QImage image_out(static_cast<int>(kMaskThumbnailSize), static_cast<int>(kMaskThumbnailSize),
                   QImage::Format_Grayscale8);
  if (image_out.isNull()) {
    throw std::runtime_error("Mask thumbnail image allocation failed");
  }
  image_out.fill(0);

  const auto content_w = static_cast<int>(geometry.render_extent.width);
  const auto content_h = static_cast<int>(geometry.render_extent.height);
  const int  origin_x  = (static_cast<int>(kMaskThumbnailSize) - content_w) / 2;
  const int  origin_y  = (static_cast<int>(kMaskThumbnailSize) - content_h) / 2;
  const float full_w   = static_cast<float>(spec.geometry.full_reference.width);
  const float full_h   = static_cast<float>(spec.geometry.full_reference.height);

  for (int y = 0; y < content_h; ++y) {
    auto* row = image_out.scanLine(origin_y + y);
    for (int x = 0; x < content_w; ++x) {
      const Vector2 render_center{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
      const Vector2 reference = TransformPoint(geometry.render_to_reference, render_center);
      if (reference.x < 0.0f || reference.y < 0.0f || reference.x >= full_w ||
          reference.y >= full_h) {
        continue;
      }
      const float nx = reference.x / full_w;
      const float ny = reference.y / full_h;
      std::uint8_t mix = 0;
      if (spec.kind == MaskThumbnailKind::Single) {
        if (spec.layers.size() != 1) {
          throw std::runtime_error("single Mask thumbnail spec must hold one layer");
        }
        const auto& layer = spec.layers.front();
        if (layer.enabled) {
          mix = ApplyInvertOpacity(LayerCoverage(layer, nx, ny), layer.invert, layer.opacity);
        }
      } else {
        for (const auto& layer : spec.layers) {
          if (!layer.enabled) {
            continue;
          }
          mix = std::max(mix, ApplyInvertOpacity(LayerCoverage(layer, nx, ny), layer.invert,
                                                 layer.opacity));
        }
      }
      row[origin_x + x] = mix;
    }
  }
  return image_out;
}

}  // namespace alcedo
