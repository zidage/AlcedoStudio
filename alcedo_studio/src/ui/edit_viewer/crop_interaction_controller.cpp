//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/crop_interaction_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace alcedo {
namespace {

constexpr float kPi = 3.14159265358979323846f;

auto            ToCropCorner(int corner_index) -> CropCorner {
  switch (corner_index) {
    case 0:
      return CropCorner::TopLeft;
    case 1:
      return CropCorner::TopRight;
    case 2:
      return CropCorner::BottomRight;
    case 3:
      return CropCorner::BottomLeft;
    default:
      return CropCorner::None;
  }
}

}  // namespace

auto CropInteractionController::HandlePress(ViewerState&              state,
                                            const ViewportWidgetInfo& widget_info,
                                            const ViewportImageInfo&  image_info,
                                            const QPointF& event_pos) -> CropInteractionResult {
  const auto       crop_state = state.GetCropOverlay();
  const auto       view_state = state.GetViewTransform();

  CropPressContext press_context;
  press_context.event_pos = event_pos;
  press_context.image_uv  = ViewportMapper::WidgetPointToImageUv(event_pos, widget_info, image_info,
                                                                 view_state.zoom, view_state.pan);
  press_context.inside_image = press_context.image_uv.has_value();

  if (crop_state.tool_enabled && crop_state.overlay_visible) {
    const float metric_aspect =
        CropGeometry::SafeAspect(image_info.image_width, image_info.image_height);
    const auto crop_corners_uv = CropGeometry::RotatedCropCornersUv(
        crop_state.rect, crop_state.rotation_degrees, metric_aspect);
    std::array<QPointF, 4> crop_corners_widget{};
    bool                   corners_valid = true;
    for (int i = 0; i < static_cast<int>(crop_corners_uv.size()); ++i) {
      const auto corner_widget =
          ViewportMapper::ImageUvToWidgetPoint(crop_corners_uv[static_cast<size_t>(i)], widget_info,
                                               image_info, view_state.zoom, view_state.pan);
      if (!corner_widget.has_value()) {
        corners_valid = false;
        break;
      }
      crop_corners_widget[static_cast<size_t>(i)] = *corner_widget;
    }
    if (corners_valid) {
      press_context.hit_test = CropGeometry::HitTestWidgetGeometry(crop_corners_widget, event_pos);
      if (press_context.image_uv.has_value()) {
        press_context.hit_test.inside_crop = CropGeometry::IsPointInsideRotatedCrop(
            *press_context.image_uv, crop_state.rect, crop_state.rotation_degrees, metric_aspect);
      }
    }
  }

  return HandlePress(state, image_info, press_context);
}

auto CropInteractionController::HandlePress(ViewerState& state, const ViewportImageInfo& image_info,
                                            const CropPressContext& press_context)
    -> CropInteractionResult {
  CropInteractionResult result;
  const auto            crop_state = state.GetCropOverlay();
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    return result;
  }

  const float metric_aspect =
      CropGeometry::SafeAspect(image_info.image_width, image_info.image_height);
  const auto crop_corners_uv = CropGeometry::RotatedCropCornersUv(
      crop_state.rect, crop_state.rotation_degrees, metric_aspect);
  CropHitTestResult hit_test = press_context.hit_test;
  if (!hit_test.rotate_handle_hit && !press_context.inside_image) {
    result.consumed = true;
    return result;
  }

  const QPointF uv_point =
      press_context.image_uv.has_value()
          ? QPointF(CropGeometry::Clamp01(static_cast<float>(press_context.image_uv->x())),
                    CropGeometry::Clamp01(static_cast<float>(press_context.image_uv->y())))
          : QPointF();

  CropOverlayState new_state = crop_state;
  new_state.metric_aspect    = metric_aspect;
  drag_anchor_uv_            = uv_point;
  drag_anchor_widget_pos_    = press_context.event_pos;
  drag_origin_rect_          = crop_state.rect;
  drag_pre_press_rect_       = crop_state.rect;
  drag_pre_press_rotation_   = crop_state.rotation_degrees;
  drag_rotation_degrees_     = crop_state.rotation_degrees;
  drag_corner_               = CropCorner::None;
  drag_edge_                 = CropEdge::None;
  drag_fixed_corner_uv_      = QPointF();

  if (hit_test.rotate_handle_hit) {
    drag_mode_    = CropDragMode::RotateHandle;
    result.cursor = Qt::ClosedHandCursor;
  } else if (hit_test.corner_index >= 0) {
    drag_mode_                = CropDragMode::ResizeCorner;
    drag_corner_              = ToCropCorner(hit_test.corner_index);
    const int opposite_corner = CropGeometry::OppositeCropCornerIndex(hit_test.corner_index);
    if (opposite_corner >= 0) {
      drag_fixed_corner_uv_ = crop_corners_uv[static_cast<size_t>(opposite_corner)];
    }
    result.cursor = CropGeometry::CursorForCropCorner(hit_test.corner_index);
  } else if (hit_test.edge != CropEdge::None) {
    drag_mode_    = CropDragMode::ResizeEdge;
    drag_edge_    = hit_test.edge;
    result.cursor = Qt::SizeAllCursor;
  } else if (hit_test.inside_crop) {
    drag_mode_    = CropDragMode::Move;
    result.cursor = Qt::SizeAllCursor;
  } else {
    drag_mode_     = CropDragMode::Create;
    new_state.rect = CropGeometry::ClampCropRectForRotation(
        QRectF(uv_point, QSizeF(CropGeometry::kCropMinSize, CropGeometry::kCropMinSize)),
        crop_state.rotation_degrees, metric_aspect);
    drag_origin_rect_ = new_state.rect;
    result.cursor     = Qt::CrossCursor;
  }

  state.SetCropOverlayState(new_state);
  result.consumed        = true;
  result.request_repaint = true;
  result.rect_changed    = new_state.rect;
  return result;
}

auto CropInteractionController::HandleMove(ViewerState&              state,
                                           const ViewportWidgetInfo& widget_info,
                                           const ViewportImageInfo&  image_info,
                                           Qt::MouseButtons buttons, const QPointF& event_pos)
    -> CropInteractionResult {
  CropInteractionResult result;
  if ((buttons & Qt::LeftButton) != Qt::LeftButton || drag_mode_ == CropDragMode::None) {
    return result;
  }

  CropOverlayState crop_state = state.GetCropOverlay();
  const auto       view_state = state.GetViewTransform();
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    return result;
  }

  const float metric_aspect =
      CropGeometry::SafeAspect(image_info.image_width, image_info.image_height);
  QPointF uv{};
  if (drag_mode_ != CropDragMode::RotateHandle) {
    const auto uv_opt = ViewportMapper::WidgetPointToImageUv(event_pos, widget_info, image_info,
                                                             view_state.zoom, view_state.pan);
    if (!uv_opt.has_value()) {
      result.consumed = true;
      return result;
    }
    uv = QPointF(CropGeometry::Clamp01(static_cast<float>(uv_opt->x())),
                 CropGeometry::Clamp01(static_cast<float>(uv_opt->y())));
  }

  QRectF new_rect             = drag_origin_rect_;
  float  new_rotation_degrees = drag_rotation_degrees_;
  bool   rotation_changed     = false;
  if (drag_mode_ == CropDragMode::Create) {
    const QRectF draft_rect = crop_state.aspect_locked
                                  ? CropGeometry::MakeAspectLockedRectFromDiagonal(
                                        drag_anchor_uv_, uv, metric_aspect, crop_state.aspect_ratio)
                                  : QRectF(drag_anchor_uv_, uv).normalized();
    new_rect =
        CropGeometry::ClampCropRectForRotation(draft_rect, drag_rotation_degrees_, metric_aspect);
  } else if (drag_mode_ == CropDragMode::Move) {
    const QPointF delta_metric = CropGeometry::UvToMetric(uv, metric_aspect) -
                                 CropGeometry::UvToMetric(drag_anchor_uv_, metric_aspect);
    const QPointF new_center_metric =
        CropGeometry::UvToMetric(drag_origin_rect_.center(), metric_aspect) + delta_metric;
    const QPointF new_center_uv = CropGeometry::MetricToUv(new_center_metric, metric_aspect);
    new_rect                    = CropGeometry::ClampCropRectForRotation(
        CropGeometry::MakeRectFromCenterSize(new_center_uv,
                                                                static_cast<float>(drag_origin_rect_.width()),
                                                                static_cast<float>(drag_origin_rect_.height())),
        drag_rotation_degrees_, metric_aspect);
  } else if (drag_mode_ == CropDragMode::ResizeEdge) {
    const QPointF center_metric =
        CropGeometry::UvToMetric(drag_origin_rect_.center(), metric_aspect);
    const QPointF cursor_metric = CropGeometry::UvToMetric(uv, metric_aspect);
    const QPointF local =
        CropGeometry::InverseRotateVector(cursor_metric - center_metric, drag_rotation_degrees_);

    const float min_width_metric  = CropGeometry::kCropMinSize * metric_aspect;
    const float min_height_metric = CropGeometry::kCropMinSize;
    float       left              = -std::max((CropGeometry::kCropMinSize * metric_aspect) * 0.5f,
                                              static_cast<float>(drag_origin_rect_.width()) * metric_aspect * 0.5f);
    float       right             = std::max((CropGeometry::kCropMinSize * metric_aspect) * 0.5f,
                                             static_cast<float>(drag_origin_rect_.width()) * metric_aspect * 0.5f);
    float       top               = -std::max(CropGeometry::kCropMinSize * 0.5f,
                                              static_cast<float>(drag_origin_rect_.height()) * 0.5f);
    float       bottom            = std::max(CropGeometry::kCropMinSize * 0.5f,
                                             static_cast<float>(drag_origin_rect_.height()) * 0.5f);
    float       center_local_x    = 0.0f;
    float       center_local_y    = 0.0f;

    if (crop_state.aspect_locked) {
      const float locked_ratio = CropGeometry::ClampAspectRatio(crop_state.aspect_ratio);
      switch (drag_edge_) {
        case CropEdge::Right: {
          right = std::max(left + min_width_metric, static_cast<float>(local.x()));
          const float width_metric = std::max(min_width_metric, right - left);
          const float half_height =
              std::max(min_height_metric * 0.5f, (width_metric / locked_ratio) * 0.5f);
          center_local_x = (left + right) * 0.5f;
          top            = -half_height;
          bottom         = half_height;
          break;
        }
        case CropEdge::Left: {
          left = std::min(right - min_width_metric, static_cast<float>(local.x()));
          const float width_metric = std::max(min_width_metric, right - left);
          const float half_height =
              std::max(min_height_metric * 0.5f, (width_metric / locked_ratio) * 0.5f);
          center_local_x = (left + right) * 0.5f;
          top            = -half_height;
          bottom         = half_height;
          break;
        }
        case CropEdge::Top: {
          top = std::min(bottom - min_height_metric, static_cast<float>(local.y()));
          const float height_metric = std::max(min_height_metric, bottom - top);
          const float half_width =
              std::max(min_width_metric * 0.5f, (height_metric * locked_ratio) * 0.5f);
          center_local_y = (top + bottom) * 0.5f;
          left           = -half_width;
          right          = half_width;
          break;
        }
        case CropEdge::Bottom: {
          bottom = std::max(top + min_height_metric, static_cast<float>(local.y()));
          const float height_metric = std::max(min_height_metric, bottom - top);
          const float half_width =
              std::max(min_width_metric * 0.5f, (height_metric * locked_ratio) * 0.5f);
          center_local_y = (top + bottom) * 0.5f;
          left           = -half_width;
          right          = half_width;
          break;
        }
        default:
          break;
      }
    } else {
      switch (drag_edge_) {
        case CropEdge::Right:
          right          = std::max(left + min_width_metric, static_cast<float>(local.x()));
          center_local_x = (left + right) * 0.5f;
          break;
        case CropEdge::Left:
          left           = std::min(right - min_width_metric, static_cast<float>(local.x()));
          center_local_x = (left + right) * 0.5f;
          break;
        case CropEdge::Top:
          top            = std::min(bottom - min_height_metric, static_cast<float>(local.y()));
          center_local_y = (top + bottom) * 0.5f;
          break;
        case CropEdge::Bottom:
          bottom         = std::max(top + min_height_metric, static_cast<float>(local.y()));
          center_local_y = (top + bottom) * 0.5f;
          break;
        default:
          break;
      }
    }

    const float new_half_width =
        std::max((CropGeometry::kCropMinSize * metric_aspect) * 0.5f, (right - left) * 0.5f);
    const float new_half_height =
        std::max(CropGeometry::kCropMinSize * 0.5f, (bottom - top) * 0.5f);
    const QPointF center_shift_local(center_local_x, center_local_y);
    const QPointF new_center_metric =
        center_metric + CropGeometry::RotateVector(center_shift_local, drag_rotation_degrees_);
    const QPointF new_center_uv = CropGeometry::MetricToUv(new_center_metric, metric_aspect);
    const float   new_width_uv =
        std::max(CropGeometry::kCropMinSize, (new_half_width * 2.0f) / metric_aspect);
    const float new_height_uv = std::max(CropGeometry::kCropMinSize, new_half_height * 2.0f);
    new_rect                  = CropGeometry::ClampCropRectForRotation(
        CropGeometry::MakeRectFromCenterSize(new_center_uv, new_width_uv, new_height_uv),
        drag_rotation_degrees_, metric_aspect);
  } else if (drag_mode_ == CropDragMode::ResizeCorner) {
    new_rect = CropGeometry::ResizeRotatedCropFromFixedCorner(
        drag_fixed_corner_uv_, uv, drag_rotation_degrees_, metric_aspect, crop_state.aspect_locked,
        crop_state.aspect_ratio);
  } else if (drag_mode_ == CropDragMode::RotateHandle) {
    const auto center_widget = ViewportMapper::ImageUvToWidgetPoint(
        drag_origin_rect_.center(), widget_info, image_info, view_state.zoom, view_state.pan);
    if (!center_widget.has_value()) {
      result.consumed = true;
      return result;
    }
    const QPointF start_vector   = drag_anchor_widget_pos_ - *center_widget;
    const QPointF current_vector = event_pos - *center_widget;
    const float   start_len2 = static_cast<float>(QPointF::dotProduct(start_vector, start_vector));
    const float   current_len2 =
        static_cast<float>(QPointF::dotProduct(current_vector, current_vector));
    if (start_len2 <= 1e-8f || current_len2 <= 1e-8f) {
      result.consumed = true;
      return result;
    }
    const float start_angle =
        std::atan2(static_cast<float>(start_vector.y()), static_cast<float>(start_vector.x()));
    const float current_angle =
        std::atan2(static_cast<float>(current_vector.y()), static_cast<float>(current_vector.x()));
    const float delta_degrees =
        CropGeometry::NormalizeAngleDegrees((current_angle - start_angle) * (180.0f / kPi));
    new_rotation_degrees = std::clamp(drag_rotation_degrees_ + delta_degrees, -180.0f, 180.0f);
    new_rect = CropGeometry::ClampCropRectForRotation(drag_origin_rect_, new_rotation_degrees,
                                                      metric_aspect);
    rotation_changed = true;
  }

  crop_state.metric_aspect = metric_aspect;
  crop_state.rect          = new_rect;
  if (rotation_changed) {
    crop_state.rotation_degrees = new_rotation_degrees;
  }
  state.SetCropOverlayState(crop_state);

  result.consumed        = true;
  result.request_repaint = true;
  result.rect_changed    = new_rect;
  if (rotation_changed) {
    result.rotation_changed = new_rotation_degrees;
  }
  return result;
}

auto CropInteractionController::HandleRelease(ViewerState& state) -> CropInteractionResult {
  CropInteractionResult result;
  if (drag_mode_ == CropDragMode::None) {
    return result;
  }

  const auto crop_state = state.GetCropOverlay();
  result.consumed       = true;
  result.unset_cursor   = true;
  result.rect_changed   = crop_state.rect;
  result.rect_is_final  = true;
  if (drag_mode_ == CropDragMode::RotateHandle) {
    result.rotation_changed  = crop_state.rotation_degrees;
    result.rotation_is_final = true;
  }
  Cancel();
  return result;
}

auto CropInteractionController::HandleDoubleClick(ViewerState& state) -> CropInteractionResult {
  CropInteractionResult result;
  auto                  crop_state = state.GetCropOverlay();
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    return result;
  }

  crop_state.rotation_degrees = 0.0f;
  crop_state.aspect_locked    = false;
  crop_state.aspect_ratio     = 1.0f;
  crop_state.rect             = QRectF(0.0, 0.0, 1.0, 1.0);
  state.SetCropOverlayState(crop_state);

  result.consumed          = true;
  result.request_repaint   = true;
  result.rect_changed      = crop_state.rect;
  result.rect_is_final     = true;
  result.rotation_changed  = crop_state.rotation_degrees;
  result.rotation_is_final = true;
  return result;
}

void CropInteractionController::Cancel(ViewerState& state) {
  if (drag_mode_ != CropDragMode::None) {
    auto crop_state = state.GetCropOverlay();
    crop_state.rect = drag_pre_press_rect_;
    crop_state.rotation_degrees = drag_pre_press_rotation_;
    state.SetCropOverlayState(crop_state);
  }
  Cancel();
}

void CropInteractionController::Cancel() {
  drag_mode_              = CropDragMode::None;
  drag_corner_            = CropCorner::None;
  drag_edge_              = CropEdge::None;
  drag_rotation_degrees_  = 0.0f;
  drag_fixed_corner_uv_   = QPointF();
  drag_anchor_widget_pos_ = QPointF();
}

auto CropInteractionController::MakeRectEmissionResult(const QRectF& rect, bool is_final) const
    -> CropInteractionResult {
  CropInteractionResult result;
  result.rect_changed  = rect;
  result.rect_is_final = is_final;
  return result;
}

}  // namespace alcedo
