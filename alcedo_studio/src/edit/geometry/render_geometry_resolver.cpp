//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/geometry/render_geometry_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/geometry/crop_frame.hpp"

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr auto  kMaxPixelExtent = std::numeric_limits<std::int32_t>::max();

/// Require source dimensions that the signed pixel rectangles can represent.
void            ThrowIfInvalidExtent(const Extent2D& extent, const char* name) {
  if (extent.Empty()) {
    throw std::runtime_error(std::string("ResolveRenderGeometry: ") + name + " must be positive");
  }
  if (extent.width > static_cast<std::uint32_t>(kMaxPixelExtent) ||
      extent.height > static_cast<std::uint32_t>(kMaxPixelExtent)) {
    throw std::runtime_error(std::string("ResolveRenderGeometry: ") + name +
                             " exceeds the signed pixel range");
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

/// Round a positive size to at least one pixel without exceeding the signed pixel range.
auto RoundExtent(double width, double height) -> Extent2D {
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
    throw std::runtime_error(
        "ResolveRenderGeometry: extent components must be positive and finite");
  }
  if (width > kMaxPixelExtent || height > kMaxPixelExtent) {
    throw std::runtime_error("ResolveRenderGeometry: extent exceeds the signed pixel range");
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

/// Expand sorted bounds by a sampling radius, clamp to the source, and keep at least one pixel.
auto RequiredPixelSpan(float low, float high, float radius, std::uint32_t extent)
    -> std::pair<std::int32_t, std::int32_t> {
  const auto limit = static_cast<double>(extent);
  const auto lower = std::clamp(static_cast<double>(low) - radius, 0.0, limit);
  const auto upper = std::clamp(static_cast<double>(high) + radius, 0.0, limit);
  auto       first = static_cast<std::int32_t>(std::floor(lower));
  auto       last  = static_cast<std::int32_t>(std::ceil(upper));
  if (first == last) {
    if (first > 0) {
      --first;
    } else {
      ++last;
    }
  }
  return {first, last};
}

auto RequiredRegionFromAabb(const Aabb& box, float radius_x, float radius_y, Extent2D extent)
    -> RectI {
  const auto [x0, x1] = RequiredPixelSpan(box.min_x, box.max_x, radius_x, extent.width);
  const auto [y0, y1] = RequiredPixelSpan(box.min_y, box.max_y, radius_y, extent.height);
  return RectI{x0, y0, x1 - x0, y1 - y0};
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

/// Map the framed canvas edges [0, width] x [0, height] to their oriented coordinates.
/// Using edge coordinates maps every half-integer pixel center to another pixel center.
auto FrameOrientationTransform(FrameOrientation orientation, Extent2D extent) -> Matrix3x3 {
  const auto width  = static_cast<float>(extent.width);
  const auto height = static_cast<float>(extent.height);
  switch (orientation) {
    case FrameOrientation::Identity:
      return Matrix3x3::Identity();
    case FrameOrientation::Rotate90Clockwise:
      return Matrix3x3{{0, -1, height, 1, 0, 0, 0, 0, 1}};
    case FrameOrientation::Rotate180:
      return Matrix3x3{{-1, 0, width, 0, -1, height, 0, 0, 1}};
    case FrameOrientation::Rotate90Counterclockwise:
      return Matrix3x3{{0, 1, 0, -1, 0, width, 0, 0, 1}};
    case FrameOrientation::FlipHorizontal:
      return Matrix3x3{{-1, 0, width, 0, 1, 0, 0, 0, 1}};
    case FrameOrientation::FlipVertical:
      return Matrix3x3{{1, 0, 0, 0, -1, height, 0, 0, 1}};
    case FrameOrientation::Transpose:
      return Matrix3x3{{0, 1, 0, 1, 0, 0, 0, 0, 1}};
    case FrameOrientation::Transverse:
      return Matrix3x3{{0, -1, height, -1, 0, width, 0, 0, 1}};
  }
  throw std::runtime_error("ResolveRenderGeometry: invalid frame orientation");
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

  // The preview frame reads the whole source; only the product crop frame is constrained to
  // keep its rotated corners inside the source.
  const auto crop =
      image.output_frame == GeometryOutputFrame::CropFrame
          ? ClampCropToRotatedSource(image.crop_rect, image.rotation_degrees,
                                     source.full_reference_extent)
          : ClampNormalizedRect(image.crop_rect, "crop_rect");
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
  const auto             centered  = rotate * to_center;

  ResolvedRenderGeometry geometry;
  geometry.decoded_extent        = source.decoded_extent;
  geometry.full_reference_extent = source.full_reference_extent;
  geometry.decoded_to_reference =
      MakeDecodedToReference(source.decoded_extent, source.full_reference_extent);

  if (image.output_frame == GeometryOutputFrame::RotatedSourceBounds) {
    const auto  rotated_box = AabbOfTransformedCorners(centered, {left, top}, {right, top},
                                                       {right, bottom}, {left, bottom});
    const float aabb_w =
        std::max(rotated_box.max_x - rotated_box.min_x, kGeometryMinNormalizedSize);
    const float aabb_h =
        std::max(rotated_box.max_y - rotated_box.min_y, kGeometryMinNormalizedSize);
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

  geometry.orientation = image.orientation;
  if (image.orientation != FrameOrientation::Identity) {
    const auto orient          = FrameOrientationTransform(image.orientation, geometry.edit_extent);
    geometry.reference_to_edit = orient * geometry.reference_to_edit;
    if (orient.m[0] == 0.0f) {
      std::swap(geometry.edit_extent.width, geometry.edit_extent.height);
    }
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

  if (footprint.requires_full_reference) {
    geometry.required_decoded_region =
        RectI{0, 0, static_cast<std::int32_t>(geometry.decoded_extent.width),
              static_cast<std::int32_t>(geometry.decoded_extent.height)};
    geometry.required_reference_region =
        RectI{0, 0, static_cast<std::int32_t>(geometry.full_reference_extent.width),
              static_cast<std::int32_t>(geometry.full_reference_extent.height)};
  } else {
    const float render_w = static_cast<float>(geometry.render_extent.width);
    const float render_h = static_cast<float>(geometry.render_extent.height);
    const auto  decoded_box =
        AabbOfTransformedCorners(geometry.render_to_decoded, {0.0f, 0.0f}, {render_w, 0.0f},
                                 {render_w, render_h}, {0.0f, render_h});
    const auto reference_box =
        AabbOfTransformedCorners(geometry.render_to_reference, {0.0f, 0.0f}, {render_w, 0.0f},
                                 {render_w, render_h}, {0.0f, render_h});
    geometry.required_decoded_region = RequiredRegionFromAabb(
        decoded_box, footprint.radius_x, footprint.radius_y, geometry.decoded_extent);
    geometry.required_reference_region = RequiredRegionFromAabb(
        reference_box, footprint.radius_x, footprint.radius_y, geometry.full_reference_extent);
  }

  FillGpuData(geometry);
  return geometry;
}

}  // namespace alcedo
