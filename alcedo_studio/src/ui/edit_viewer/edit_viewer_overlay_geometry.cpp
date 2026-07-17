//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"

#include <algorithm>

namespace alcedo {
namespace {

auto ResolveZoomPan(const EditViewerOverlaySnapshot& snapshot) -> std::pair<float, QVector2D> {
  float     zoom = snapshot.viewer_state.view_transform.zoom;
  QVector2D pan  = snapshot.viewer_state.view_transform.pan;
  if (snapshot.presentation_mode == FramePresentationMode::RoiFrame) {
    zoom = 1.0f;
    pan  = QVector2D(0.0f, 0.0f);
  }
  return {zoom, pan};
}

}  // namespace

auto EditViewerOverlayGeometry::Build(const EditViewerOverlaySnapshot& snapshot)
    -> CropOverlayWidgetGeometry {
  CropOverlayWidgetGeometry geometry;
  if (snapshot.image_info.image_width <= 0 || snapshot.image_info.image_height <= 0) {
    return geometry;
  }

  const auto [zoom, pan] = ResolveZoomPan(snapshot);

  const auto image_top_left = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(0.0, 0.0), snapshot.widget_info, snapshot.image_info, zoom, pan);
  const auto image_bottom_right = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(1.0, 1.0), snapshot.widget_info, snapshot.image_info, zoom, pan);
  if (image_top_left.has_value() && image_bottom_right.has_value()) {
    geometry.image_rect       = QRectF(*image_top_left, *image_bottom_right).normalized();
    geometry.image_rect_valid = geometry.image_rect.isValid();
  }

  // Detail ROI is independent of crop overlay visibility.
  if (snapshot.detail_roi_visible && snapshot.detail_roi_uv.isValid()) {
    const QRectF roi = snapshot.detail_roi_uv.normalized();
    const std::array<QPointF, 4> roi_uv = {
        QPointF(roi.left(), roi.top()),
        QPointF(roi.right(), roi.top()),
        QPointF(roi.right(), roi.bottom()),
        QPointF(roi.left(), roi.bottom()),
    };
    bool roi_ok = true;
    for (size_t i = 0; i < roi_uv.size(); ++i) {
      const auto corner = ViewportMapper::ImageUvToWidgetPoint(
          roi_uv[i], snapshot.widget_info, snapshot.image_info, zoom, pan);
      if (!corner.has_value()) {
        roi_ok = false;
        break;
      }
      geometry.detail_roi_corners_widget[i] = *corner;
    }
    geometry.detail_roi_valid = roi_ok;
  }

  const auto& crop_state = snapshot.viewer_state.crop_overlay;
  if (!crop_state.overlay_visible) {
    return geometry;
  }

  const auto crop_corners_uv = CropGeometry::RotatedCropCornersUv(
      crop_state.rect, crop_state.rotation_degrees, crop_state.metric_aspect);
  for (size_t i = 0; i < crop_corners_uv.size(); ++i) {
    const auto corner_widget = ViewportMapper::ImageUvToWidgetPoint(
        crop_corners_uv[i], snapshot.widget_info, snapshot.image_info, zoom, pan);
    if (!corner_widget.has_value()) {
      geometry.crop_corners_valid = false;
      return geometry;
    }
    geometry.crop_corners_widget[i] = *corner_widget;
  }

  geometry.crop_corners_valid = true;
  const auto handle_points =
      CropGeometry::CropRotateHandleWidgetPoint(geometry.crop_corners_widget);
  geometry.rotate_stem_widget   = handle_points.first;
  geometry.rotate_handle_widget = handle_points.second;
  return geometry;
}

auto EditViewerOverlayGeometry::ComputeHover(const EditViewerOverlaySnapshot& snapshot,
                                             const CropOverlayWidgetGeometry& geometry,
                                             const QPointF& event_pos) -> EditViewerOverlayHover {
  EditViewerOverlayHover hover;
  if (snapshot.image_info.image_width <= 0 || snapshot.image_info.image_height <= 0) {
    return hover;
  }

  const auto [zoom, pan] = ResolveZoomPan(snapshot);
  hover.image_uv = ViewportMapper::WidgetPointToImageUv(event_pos, snapshot.widget_info,
                                                        snapshot.image_info, zoom, pan);
  hover.inside_image = hover.image_uv.has_value();
  if (!hover.inside_image) {
    hover.kind = EditViewerOverlayHitKind::OutsideImage;
    return hover;
  }

  const auto& crop_state = snapshot.viewer_state.crop_overlay;
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    hover.kind = EditViewerOverlayHitKind::BlankInImage;
    return hover;
  }

  if (geometry.crop_corners_valid) {
    hover.crop_hit = CropGeometry::HitTestWidgetGeometry(geometry.crop_corners_widget, event_pos);
    hover.crop_hit.inside_crop =
        CropGeometry::IsPointInsideRotatedCrop(*hover.image_uv, crop_state.rect,
                                               crop_state.rotation_degrees,
                                               crop_state.metric_aspect);

    if (hover.crop_hit.rotate_handle_hit) {
      hover.kind   = EditViewerOverlayHitKind::RotateHandle;
      hover.cursor = Qt::OpenHandCursor;
      return hover;
    }
    if (hover.crop_hit.corner_index >= 0) {
      hover.kind   = EditViewerOverlayHitKind::Corner;
      hover.cursor = CropGeometry::CursorForCropCorner(hover.crop_hit.corner_index);
      return hover;
    }
    if (hover.crop_hit.edge != CropEdge::None) {
      hover.kind   = EditViewerOverlayHitKind::Edge;
      hover.cursor = Qt::SizeAllCursor;
      return hover;
    }
    if (hover.crop_hit.inside_crop) {
      hover.kind   = EditViewerOverlayHitKind::InsideCrop;
      hover.cursor = Qt::SizeAllCursor;
      return hover;
    }
  }

  hover.kind   = EditViewerOverlayHitKind::BlankInImage;
  hover.cursor = Qt::CrossCursor;
  return hover;
}

}  // namespace alcedo
