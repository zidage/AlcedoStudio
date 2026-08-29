//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/geometry/render_geometry_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void            ThrowIfInvalidExtent(const Extent2D& extent, const char* name) {
  if (extent.Empty()) {
    throw std::runtime_error(std::string("ResolveRenderGeometry: ") + name + " must be positive");
  }
}

auto ClampNormalizedRect(NormalizedRect rect, const char* name) -> NormalizedRect {
  if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) ||
      !std::isfinite(rect.h)) {
    throw std::runtime_error(std::string("ResolveRenderGeometry: ") + name + " must be finite");
  }
  rect.w = std::clamp(rect.w, kGeometryMinNormalizedSize, 1.0f);
  rect.h = std::clamp(rect.h, kGeometryMinNormalizedSize, 1.0f);
  rect.x = std::clamp(rect.x, 0.0f, 1.0f - rect.w);
  rect.y = std::clamp(rect.y, 0.0f, 1.0f - rect.h);
  return rect;
}

auto NormalizeRotationDegrees(float degrees) -> float {
  if (!std::isfinite(degrees)) {
    return 0.0f;
  }
  degrees = std::fmod(degrees, 360.0f);
  if (degrees > 180.0f) {
    degrees -= 360.0f;
  } else if (degrees < -180.0f) {
    degrees += 360.0f;
  }
  return degrees;
}

auto RoundExtent(float width, float height) -> Extent2D {
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
    throw std::runtime_error(
        "ResolveRenderGeometry: extent components must be positive and finite");
  }
  const auto rounded_w = static_cast<std::uint32_t>(std::max(1L, std::lround(width)));
  const auto rounded_h = static_cast<std::uint32_t>(std::max(1L, std::lround(height)));
  return Extent2D{rounded_w, rounded_h};
}

auto ClampMaxEdge(Extent2D extent, std::uint32_t max_edge) -> Extent2D {
  if (max_edge == 0) {
    return extent;
  }
  const auto long_edge = std::max(extent.width, extent.height);
  if (long_edge <= max_edge) {
    return extent;
  }
  const float scale = static_cast<float>(max_edge) / static_cast<float>(long_edge);
  return RoundExtent(static_cast<float>(extent.width) * scale,
                     static_cast<float>(extent.height) * scale);
}

auto ClampRoiRenderExtentToNativePixels(Extent2D requested, float visible_width,
                                        float visible_height) -> Extent2D {
  const auto native = RoundExtent(visible_width, visible_height);
  if (requested.width >= native.width && requested.height >= native.height) {
    return native;
  }
  const float scale =
      std::min({1.0f, static_cast<float>(requested.width) / static_cast<float>(native.width),
                static_cast<float>(requested.height) / static_cast<float>(native.height)});
  return RoundExtent(static_cast<float>(native.width) * scale,
                     static_cast<float>(native.height) * scale);
}

struct Aabb {
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
};

auto AabbOfTransformedCorners(const Matrix3x3& matrix, Vector2 a, Vector2 b, Vector2 c, Vector2 d)
    -> Aabb {
  const Vector2 p[4] = {TransformPoint(matrix, a), TransformPoint(matrix, b),
                        TransformPoint(matrix, c), TransformPoint(matrix, d)};
  Aabb          box{p[0].x, p[0].y, p[0].x, p[0].y};
  for (int i = 1; i < 4; ++i) {
    box.min_x = std::min(box.min_x, p[i].x);
    box.min_y = std::min(box.min_y, p[i].y);
    box.max_x = std::max(box.max_x, p[i].x);
    box.max_y = std::max(box.max_y, p[i].y);
  }
  return box;
}

auto ClampRectI(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, Extent2D extent)
    -> RectI {
  const auto max_x = static_cast<std::int32_t>(extent.width);
  const auto max_y = static_cast<std::int32_t>(extent.height);
  x0               = std::clamp(x0, 0, max_x);
  x1               = std::clamp(x1, 0, max_x);
  y0               = std::clamp(y0, 0, max_y);
  y1               = std::clamp(y1, 0, max_y);
  if (x1 < x0) {
    std::swap(x0, x1);
  }
  if (y1 < y0) {
    std::swap(y0, y1);
  }
  if (x1 == x0) {
    if (x0 > 0) {
      --x0;
    } else if (x1 < max_x) {
      ++x1;
    }
  }
  if (y1 == y0) {
    if (y0 > 0) {
      --y0;
    } else if (y1 < max_y) {
      ++y1;
    }
  }
  return RectI{x0, y0, x1 - x0, y1 - y0};
}

auto RequiredRegionFromAabb(const Aabb& box, float radius_x, float radius_y, Extent2D extent)
    -> RectI {
  const auto x0 = static_cast<std::int32_t>(std::floor(box.min_x - radius_x));
  const auto y0 = static_cast<std::int32_t>(std::floor(box.min_y - radius_y));
  const auto x1 = static_cast<std::int32_t>(std::ceil(box.max_x + radius_x));
  const auto y1 = static_cast<std::int32_t>(std::ceil(box.max_y + radius_y));
  return ClampRectI(x0, y0, x1, y1, extent);
}

void FillGpuData(ResolvedRenderGeometry& geometry) {
  for (int i = 0; i < 9; ++i) {
    geometry.gpu_data.render_to_decoded[i] = geometry.render_to_decoded.m[i];
  }
  geometry.gpu_data.decoded_width  = geometry.decoded_extent.width;
  geometry.gpu_data.decoded_height = geometry.decoded_extent.height;
  geometry.gpu_data.render_width   = geometry.render_extent.width;
  geometry.gpu_data.render_height  = geometry.render_extent.height;
  geometry.gpu_data.border_rgba[0] = 0.0f;
  geometry.gpu_data.border_rgba[1] = 0.0f;
  geometry.gpu_data.border_rgba[2] = 0.0f;
  geometry.gpu_data.border_rgba[3] = 1.0f;
  geometry.gpu_data.filter         = static_cast<std::uint32_t>(geometry.filter);
}

}  // namespace

auto ResolveRenderGeometry(const SourceGeometry& source, const ImageGeometryParams& image,
                           const ViewRequest& view, const ResolutionRequest& resolution,
                           const SamplingFootprint& footprint) -> ResolvedRenderGeometry {
  ThrowIfInvalidExtent(source.decoded_extent, "decoded_extent");
  ThrowIfInvalidExtent(source.full_reference_extent, "full_reference_extent");
  if (!std::isfinite(resolution.render_scale) || resolution.render_scale <= 0.0f) {
    throw std::runtime_error("ResolveRenderGeometry: render_scale must be positive and finite");
  }
  if (!std::isfinite(footprint.radius_x) || !std::isfinite(footprint.radius_y) ||
      footprint.radius_x < 0.0f || footprint.radius_y < 0.0f) {
    throw std::runtime_error("ResolveRenderGeometry: footprint radius must be finite and >= 0");
  }

  const auto crop = ClampNormalizedRect(image.crop_rect, "crop_rect");
  const auto visible =
      ClampNormalizedRect(view.visible_rect_in_edit_space, "visible_rect_in_edit_space");
  const float theta     = NormalizeRotationDegrees(image.rotation_degrees) * (kPi / 180.0f);

  const float full_w    = static_cast<float>(source.full_reference_extent.width);
  const float full_h    = static_cast<float>(source.full_reference_extent.height);
  const float left      = crop.x * full_w;
  const float top       = crop.y * full_h;
  const float right     = (crop.x + crop.w) * full_w;
  const float bottom    = (crop.y + crop.h) * full_h;
  const float crop_w    = right - left;
  const float crop_h    = bottom - top;
  const float cx        = 0.5f * (left + right);
  const float cy        = 0.5f * (top + bottom);

  const auto  to_center = Matrix3x3::Translate(-cx, -cy);
  const auto  rotate    = Matrix3x3::Rotate(theta);
  const auto  centered  = rotate * to_center;
  const auto  rotated_box =
      AabbOfTransformedCorners(centered, Vector2{left, top}, Vector2{right, top},
                               Vector2{right, bottom}, Vector2{left, bottom});
  const float aabb_w = std::max(rotated_box.max_x - rotated_box.min_x, kGeometryMinNormalizedSize);
  const float aabb_h = std::max(rotated_box.max_y - rotated_box.min_y, kGeometryMinNormalizedSize);

  ResolvedRenderGeometry geometry;
  geometry.decoded_extent        = source.decoded_extent;
  geometry.full_reference_extent = source.full_reference_extent;
  geometry.decoded_to_reference =
      MakeDecodedToReference(source.decoded_extent, source.full_reference_extent);

  if (image.expand_to_fit) {
    geometry.edit_extent       = RoundExtent(aabb_w, aabb_h);
    const float sx             = static_cast<float>(geometry.edit_extent.width) / aabb_w;
    const float sy             = static_cast<float>(geometry.edit_extent.height) / aabb_h;
    geometry.reference_to_edit = Matrix3x3::Scale(sx, sy) *
                                 Matrix3x3::Translate(-rotated_box.min_x, -rotated_box.min_y) *
                                 centered;
  } else {
    geometry.edit_extent = RoundExtent(crop_w, crop_h);
    const float sx       = static_cast<float>(geometry.edit_extent.width) / crop_w;
    const float sy       = static_cast<float>(geometry.edit_extent.height) / crop_h;
    geometry.reference_to_edit =
        Matrix3x3::Translate(static_cast<float>(geometry.edit_extent.width) * 0.5f,
                             static_cast<float>(geometry.edit_extent.height) * 0.5f) *
        Matrix3x3::Scale(sx, sy) * centered;
  }

  const float edit_w   = static_cast<float>(geometry.edit_extent.width);
  const float edit_h   = static_cast<float>(geometry.edit_extent.height);
  const float v_left   = visible.x * edit_w;
  const float v_top    = visible.y * edit_h;
  const float v_right  = (visible.x + visible.w) * edit_w;
  const float v_bottom = (visible.y + visible.h) * edit_h;
  const float vis_w    = std::max(v_right - v_left, kGeometryMinNormalizedSize);
  const float vis_h    = std::max(v_bottom - v_top, kGeometryMinNormalizedSize);

  Extent2D    target;
  if (view.viewport_extent.width > 0 && view.viewport_extent.height > 0) {
    target = view.viewport_extent;
  } else {
    target = RoundExtent(vis_w, vis_h);
  }
  geometry.render_extent =
      ClampMaxEdge(RoundExtent(static_cast<float>(target.width) * resolution.render_scale,
                               static_cast<float>(target.height) * resolution.render_scale),
                   resolution.max_edge);
  // Preserve the pre-DAG preview behavior for a visible subregion: the image pipeline may
  // downsample native ROI pixels, but it must not manufacture a larger patch with bilinear or
  // bicubic interpolation. When the native ROI is smaller than the physical viewport target,
  // the viewer enlarges the patch with its nearest-neighbor sampler.
  if (!visible.IsFullFrame()) {
    geometry.render_extent =
        ClampRoiRenderExtentToNativePixels(geometry.render_extent, vis_w, vis_h);
  }

  const float rsx              = static_cast<float>(geometry.render_extent.width) / vis_w;
  const float rsy              = static_cast<float>(geometry.render_extent.height) / vis_h;
  geometry.edit_to_render      = Matrix3x3::Scale(rsx, rsy) * Matrix3x3::Translate(-v_left, -v_top);

  geometry.reference_to_render = geometry.edit_to_render * geometry.reference_to_edit;
  geometry.render_to_reference = InvertAffine(geometry.reference_to_render);
  geometry.render_to_decoded =
      InvertAffine(geometry.decoded_to_reference) * geometry.render_to_reference;

  geometry.filter         = resolution.quality == RenderQuality::Export ? TextureFilter::Bicubic
                                                                        : TextureFilter::Bilinear;

  const float render_w    = static_cast<float>(geometry.render_extent.width);
  const float render_h    = static_cast<float>(geometry.render_extent.height);
  const auto  decoded_box = AabbOfTransformedCorners(
      geometry.render_to_decoded, Vector2{0.0f, 0.0f}, Vector2{render_w, 0.0f},
      Vector2{render_w, render_h}, Vector2{0.0f, render_h});
  const auto reference_box = AabbOfTransformedCorners(
      geometry.render_to_reference, Vector2{0.0f, 0.0f}, Vector2{render_w, 0.0f},
      Vector2{render_w, render_h}, Vector2{0.0f, render_h});

  if (footprint.requires_full_reference) {
    geometry.required_decoded_region =
        RectI{0, 0, static_cast<std::int32_t>(geometry.decoded_extent.width),
              static_cast<std::int32_t>(geometry.decoded_extent.height)};
    geometry.required_reference_region =
        RectI{0, 0, static_cast<std::int32_t>(geometry.full_reference_extent.width),
              static_cast<std::int32_t>(geometry.full_reference_extent.height)};
  } else {
    geometry.required_decoded_region = RequiredRegionFromAabb(
        decoded_box, footprint.radius_x, footprint.radius_y, geometry.decoded_extent);
    geometry.required_reference_region = RequiredRegionFromAabb(
        reference_box, footprint.radius_x, footprint.radius_y, geometry.full_reference_extent);
  }

  FillGpuData(geometry);
  return geometry;
}

}  // namespace alcedo
