//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/mask_edit_geometry.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

constexpr float kAffineDetEpsilon = 1.0e-12f;
constexpr float kRoiExtentEpsilon = 1.0e-6f;

[[nodiscard]] auto IsFinite(float value) -> bool { return std::isfinite(value); }

[[nodiscard]] auto IsFinitePoint(Vector2 point) -> bool {
  return IsFinite(point.x) && IsFinite(point.y);
}

[[nodiscard]] auto IsFiniteAffine(const Matrix3x3& matrix) -> bool {
  for (int i = 0; i < 6; ++i) {
    if (!IsFinite(matrix.m[i])) {
      return false;
    }
  }
  return IsFinite(matrix.m[6]) && IsFinite(matrix.m[7]) && IsFinite(matrix.m[8]);
}

[[nodiscard]] auto AffineDeterminant(const Matrix3x3& matrix) -> float {
  return matrix.m[0] * matrix.m[4] - matrix.m[1] * matrix.m[3];
}

[[nodiscard]] auto IsInvertibleAffine(const Matrix3x3& matrix) -> bool {
  if (!IsFiniteAffine(matrix)) {
    return false;
  }
  const float det = AffineDeterminant(matrix);
  return IsFinite(det) && std::fabs(det) >= kAffineDetEpsilon;
}

[[nodiscard]] auto PhotographUvInside(Vector2 uv) -> bool {
  return uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
}

[[nodiscard]] auto RoiIsUsable(const FrameRoiRect& roi) -> bool {
  return IsFinite(roi.x) && IsFinite(roi.y) && IsFinite(roi.width) && IsFinite(roi.height) &&
         roi.width > kRoiExtentEpsilon && roi.height > kRoiExtentEpsilon;
}

[[nodiscard]] auto EffectiveZoomPan(const MaskEditViewMapping& mapping)
    -> std::pair<float, QVector2D> {
  if (mapping.presentation == FramePresentationMode::RoiFrame) {
    return {1.0f, QVector2D(0.0f, 0.0f)};
  }
  return {mapping.zoom, mapping.pan};
}

[[nodiscard]] auto UsesRoiExpansion(const MaskEditViewMapping& mapping) -> bool {
  return mapping.presentation == FramePresentationMode::RoiFrame;
}

[[nodiscard]] auto ExpandToPhotographUv(const MaskEditViewMapping& mapping, Vector2 displayed_uv)
    -> Vector2 {
  if (!UsesRoiExpansion(mapping)) {
    return displayed_uv;
  }
  return Vector2{mapping.displayed_roi.x + displayed_uv.x * mapping.displayed_roi.width,
                 mapping.displayed_roi.y + displayed_uv.y * mapping.displayed_roi.height};
}

[[nodiscard]] auto PhotographUvToDisplayed(const MaskEditViewMapping& mapping,
                                              Vector2 photograph_uv) -> Vector2 {
  if (!UsesRoiExpansion(mapping)) {
    return photograph_uv;
  }
  return Vector2{(photograph_uv.x - mapping.displayed_roi.x) / mapping.displayed_roi.width,
                 (photograph_uv.y - mapping.displayed_roi.y) / mapping.displayed_roi.height};
}

[[nodiscard]] auto RenderPixelsFromPhotographUv(const ResolvedRenderGeometry& geometry,
                                                    Vector2 photograph_uv) -> Vector2 {
  return Vector2{photograph_uv.x * static_cast<float>(geometry.render_extent.width),
                 photograph_uv.y * static_cast<float>(geometry.render_extent.height)};
}

[[nodiscard]] auto PhotographUvFromRenderPixels(const ResolvedRenderGeometry& geometry,
                                                  Vector2 render_pixels) -> Vector2 {
  return Vector2{render_pixels.x / static_cast<float>(geometry.render_extent.width),
                 render_pixels.y / static_cast<float>(geometry.render_extent.height)};
}

}  // namespace

auto MaskEditGeometry::MakeIdentityPhotographGeometry(Extent2D extent) -> ResolvedRenderGeometry {
  ResolvedRenderGeometry geometry;
  geometry.decoded_extent         = extent;
  geometry.full_reference_extent   = extent;
  geometry.edit_extent             = extent;
  geometry.render_extent           = extent;
  geometry.decoded_to_reference    = Matrix3x3::Identity();
  geometry.reference_to_edit       = Matrix3x3::Identity();
  geometry.edit_to_render         = Matrix3x3::Identity();
  geometry.reference_to_render     = Matrix3x3::Identity();
  geometry.render_to_reference    = Matrix3x3::Identity();
  geometry.render_to_decoded      = Matrix3x3::Identity();
  return geometry;
}

auto MaskEditGeometry::IsValid(const MaskEditViewMapping& mapping) -> bool {
  if (mapping.widget.widget_width <= 0 || mapping.widget.widget_height <= 0 ||
      !IsFinite(mapping.widget.device_pixel_ratio) || mapping.widget.device_pixel_ratio <= 0.0f) {
    return false;
  }
  if (mapping.photograph.image_width <= 0 || mapping.photograph.image_height <= 0) {
    return false;
  }
  if (!IsFinite(mapping.zoom) || !IsFinite(mapping.pan.x()) || !IsFinite(mapping.pan.y())) {
    return false;
  }
  if (mapping.geometry.full_reference_extent.Empty() || mapping.geometry.render_extent.Empty() ||
      mapping.geometry.edit_extent.Empty()) {
    return false;
  }
  if (!IsInvertibleAffine(mapping.geometry.render_to_reference) ||
      !IsInvertibleAffine(mapping.geometry.reference_to_render)) {
    return false;
  }
  if (UsesRoiExpansion(mapping) && !RoiIsUsable(mapping.displayed_roi)) {
    return false;
  }
  return true;
}

auto MaskEditGeometry::Identity(const MaskEditViewMapping& mapping) -> MaskEditMappingIdentity {
  MaskEditMappingIdentity identity;
  identity.widget_width          = mapping.widget.widget_width;
  identity.widget_height         = mapping.widget.widget_height;
  identity.device_pixel_ratio     = mapping.widget.device_pixel_ratio;
  identity.photograph_width      = mapping.photograph.image_width;
  identity.photograph_height     = mapping.photograph.image_height;
  const auto [zoom, pan]         = EffectiveZoomPan(mapping);
  identity.zoom                   = zoom;
  identity.pan_x                 = pan.x();
  identity.pan_y                 = pan.y();
  identity.presentation           = mapping.presentation;
  if (UsesRoiExpansion(mapping)) {
    identity.roi_x      = mapping.displayed_roi.x;
    identity.roi_y      = mapping.displayed_roi.y;
    identity.roi_width  = mapping.displayed_roi.width;
    identity.roi_height = mapping.displayed_roi.height;
  }
  identity.full_reference_width  = mapping.geometry.full_reference_extent.width;
  identity.full_reference_height = mapping.geometry.full_reference_extent.height;
  identity.render_width          = mapping.geometry.render_extent.width;
  identity.render_height         = mapping.geometry.render_extent.height;
  for (int i = 0; i < 6; ++i) {
    identity.render_to_reference[i] = mapping.geometry.render_to_reference.m[i];
  }
  return identity;
}

auto MaskEditGeometry::MappingChanged(const MaskEditMappingIdentity& before,
                                      const MaskEditMappingIdentity& after) -> bool {
  return before != after;
}

auto MaskEditGeometry::MapItemToReference(const MaskEditViewMapping& mapping, const QPointF& item,
                                           bool allow_outside)
    -> std::optional<MaskReferenceSample> {
  if (!IsValid(mapping)) {
    return std::nullopt;
  }
  const auto [zoom, pan] = EffectiveZoomPan(mapping);
  const auto displayed_uv =
      ViewportMapper::WidgetPointToImageUv(item, mapping.widget, mapping.photograph, zoom, pan);
  if (!displayed_uv.has_value() || !IsFinite(static_cast<float>(displayed_uv->x())) ||
      !IsFinite(static_cast<float>(displayed_uv->y()))) {
    return std::nullopt;
  }

  MaskReferenceSample sample;
  sample.photograph_uv =
      ExpandToPhotographUv(mapping, Vector2{static_cast<float>(displayed_uv->x()),
                                             static_cast<float>(displayed_uv->y())});
  if (!IsFinitePoint(sample.photograph_uv)) {
    return std::nullopt;
  }
  sample.inside_photograph = PhotographUvInside(sample.photograph_uv);
  if (!allow_outside && !sample.inside_photograph) {
    return std::nullopt;
  }

  const auto render_pixels = RenderPixelsFromPhotographUv(mapping.geometry, sample.photograph_uv);
  sample.reference_pixels = TransformPoint(mapping.geometry.render_to_reference, render_pixels);
  if (!IsFinitePoint(sample.reference_pixels)) {
    return std::nullopt;
  }
  sample.normalized = NormalizedFromReferencePixels(
      sample.reference_pixels, mapping.geometry.full_reference_extent);
  return sample;
}

auto MaskEditGeometry::MapReferenceToItem(const MaskEditViewMapping& mapping,
                                           Vector2 reference_pixels) -> std::optional<QPointF> {
  if (!IsValid(mapping) || !IsFinitePoint(reference_pixels)) {
    return std::nullopt;
  }
  const auto render_pixels =
      TransformPoint(mapping.geometry.reference_to_render, reference_pixels);
  if (!IsFinitePoint(render_pixels)) {
    return std::nullopt;
  }
  const auto photograph_uv = PhotographUvFromRenderPixels(mapping.geometry, render_pixels);
  if (!IsFinitePoint(photograph_uv)) {
    return std::nullopt;
  }
  const auto displayed_uv = PhotographUvToDisplayed(mapping, photograph_uv);
  if (!IsFinitePoint(displayed_uv)) {
    return std::nullopt;
  }
  const auto [zoom, pan] = EffectiveZoomPan(mapping);
  return ViewportMapper::ImageUvToWidgetPoint(QPointF(displayed_uv.x, displayed_uv.y),
                                                mapping.widget, mapping.photograph, zoom, pan,
                                                false);
}

auto MaskEditGeometry::NormalizedFromReferencePixels(Vector2 reference_pixels,
                                                     Extent2D full_reference) -> Vector2 {
  if (full_reference.Empty()) {
    throw std::runtime_error("NormalizedFromReferencePixels: full_reference must be positive");
  }
  return Vector2{reference_pixels.x / static_cast<float>(full_reference.width),
                 reference_pixels.y / static_cast<float>(full_reference.height)};
}

auto MaskEditGeometry::ReferencePixelsFromNormalized(Vector2 normalized, Extent2D full_reference)
    -> Vector2 {
  if (full_reference.Empty()) {
    throw std::runtime_error("ReferencePixelsFromNormalized: full_reference must be positive");
  }
  return Vector2{normalized.x * static_cast<float>(full_reference.width),
                 normalized.y * static_cast<float>(full_reference.height)};
}

auto MaskEditGeometry::HitsHandle(const QPointF& item, const QPointF& handle_item,
                                  float radius_logical_px) -> bool {
  if (!IsFinite(radius_logical_px) || radius_logical_px <= 0.0f) {
    return false;
  }
  const float dx = static_cast<float>(item.x() - handle_item.x());
  const float dy = static_cast<float>(item.y() - handle_item.y());
  return std::hypot(dx, dy) <= radius_logical_px;
}

}  // namespace alcedo
