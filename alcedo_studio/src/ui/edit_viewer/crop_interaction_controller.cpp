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

auto            BoxCorner(const QRectF& box, int corner_index) -> QPointF {
  switch (corner_index) {
    case 0:
      return box.topLeft();
    case 1:
      return box.topRight();
    case 2:
      return box.bottomRight();
    case 3:
      return box.bottomLeft();
    default:
      return box.center();
  }
}

}  // namespace

auto CropInteractionController::CropCornersWidget(const CropOverlayState&    crop,
                                                  const MaskEditViewMapping& mapping)
    -> std::optional<std::array<QPointF, 4>> {
  const Extent2D source = mapping.geometry.full_reference_extent;
  if (source.Empty() || !MaskEditGeometry::IsValid(mapping)) {
    return std::nullopt;
  }
  const auto corners_reference =
      CropGeometry::CropCornersInReference(crop.rect, crop.rotation_degrees, source);
  std::array<QPointF, 4> corners{};
  for (size_t i = 0; i < corners.size(); ++i) {
    const auto item = MaskEditGeometry::MapReferenceToItem(mapping, corners_reference[i]);
    if (!item.has_value()) {
      return std::nullopt;
    }
    corners[i] = *item;
  }
  return corners;
}

auto CropInteractionController::HandlePress(ViewerState& state, const MaskEditViewMapping& mapping,
                                            const QPointF& event_pos) -> CropInteractionResult {
  CropInteractionResult result;
  const auto            crop_state = state.GetCropOverlay();
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    return result;
  }
  // The crop tool owns left presses while it is visible, including presses it ignores.
  result.consumed      = true;
  const auto corners   = CropCornersWidget(crop_state, mapping);
  if (!corners.has_value()) {
    return result;
  }
  CropHitTestResult hit = CropGeometry::HitTestWidgetGeometry(*corners, event_pos);
  hit.inside_crop       = CropGeometry::IsPointInsideQuad(*corners, event_pos);
  const auto press_reference =
      MaskEditGeometry::MapItemToReference(mapping, event_pos, /*allow_outside=*/false);
  if (!hit.rotate_handle_hit && !press_reference.has_value()) {
    return result;
  }

  const float angle          = crop_state.rotation_degrees;
  drag_source_               = mapping.geometry.full_reference_extent;
  drag_origin_box_           = CropGeometry::FrameBoxFromCrop(crop_state.rect, angle, drag_source_);
  drag_anchor_frame_         = press_reference.has_value()
                                   ? CropGeometry::ReferenceToFrame(
                                         press_reference->reference_pixels, angle)
                                   : QPointF();
  drag_anchor_widget_pos_    = event_pos;
  drag_center_widget_pos_    = CropGeometry::CropCenterWidgetPoint(*corners);
  drag_pre_press_rect_       = crop_state.rect;
  drag_pre_press_rotation_   = angle;
  drag_edge_                 = CropEdge::None;
  drag_changed_              = false;

  const auto hit_cursor      = OverlayCursorShape(CropGeometry::CursorForCropHit(hit));
  if (hit.rotate_handle_hit) {
    drag_mode_    = CropDragMode::RotateHandle;
    result.cursor = hit_cursor;
  } else if (hit.corner_index >= 0) {
    drag_mode_ = CropDragMode::ResizeCorner;
    drag_fixed_corner_frame_ =
        BoxCorner(drag_origin_box_, CropGeometry::OppositeCropCornerIndex(hit.corner_index));
    result.cursor = hit_cursor;
  } else if (hit.edge != CropEdge::None) {
    drag_mode_    = CropDragMode::ResizeEdge;
    drag_edge_    = hit.edge;
    result.cursor = hit_cursor;
  } else if (hit.inside_crop) {
    drag_mode_    = CropDragMode::Move;
    result.cursor = hit_cursor;
  } else {
    drag_mode_    = CropDragMode::Create;
    result.cursor = Qt::CrossCursor;
  }
  return result;
}

auto CropInteractionController::HandleMove(ViewerState& state, const MaskEditViewMapping& mapping,
                                           Qt::MouseButtons buttons, const QPointF& event_pos)
    -> CropInteractionResult {
  CropInteractionResult result;
  if ((buttons & Qt::LeftButton) != Qt::LeftButton || drag_mode_ == CropDragMode::None) {
    return result;
  }
  CropOverlayState crop_state = state.GetCropOverlay();
  if (!crop_state.tool_enabled || !crop_state.overlay_visible) {
    return result;
  }
  result.consumed = true;

  QRectF new_rect             = drag_pre_press_rect_;
  float  new_rotation_degrees = drag_pre_press_rotation_;
  bool   rotation_changed     = false;
  if (drag_mode_ == CropDragMode::RotateHandle) {
    const QPointF start_vector   = drag_anchor_widget_pos_ - drag_center_widget_pos_;
    const QPointF current_vector = event_pos - drag_center_widget_pos_;
    if (QPointF::dotProduct(start_vector, start_vector) <= 1e-8 ||
        QPointF::dotProduct(current_vector, current_vector) <= 1e-8) {
      return result;
    }
    const float start_angle =
        std::atan2(static_cast<float>(start_vector.y()), static_cast<float>(start_vector.x()));
    const float current_angle =
        std::atan2(static_cast<float>(current_vector.y()), static_cast<float>(current_vector.x()));
    const float delta_degrees =
        CropGeometry::NormalizeAngleDegrees((current_angle - start_angle) * (180.0f / kPi));
    new_rotation_degrees =
        std::clamp(drag_pre_press_rotation_ + delta_degrees, -180.0f, 180.0f);
    // The crop center stays on the same source content; the frame shrinks if needed.
    new_rect         = CropGeometry::ClampCrop(drag_pre_press_rect_, new_rotation_degrees,
                                               drag_source_);
    rotation_changed = true;
  } else {
    const auto reference =
        MaskEditGeometry::MapItemToReference(mapping, event_pos, /*allow_outside=*/true);
    if (!reference.has_value()) {
      return result;
    }
    const QPointF cursor =
        CropGeometry::ReferenceToFrame(reference->reference_pixels, drag_pre_press_rotation_);
    QRectF box = drag_origin_box_;
    switch (drag_mode_) {
      case CropDragMode::Create:
        box = crop_state.aspect_locked
                  ? CropGeometry::MakeAspectLockedBoxFromDiagonal(drag_anchor_frame_, cursor,
                                                                  crop_state.aspect_ratio)
                  : CropGeometry::ResizeBoxFromFixedCorner(drag_anchor_frame_, cursor, false,
                                                           crop_state.aspect_ratio);
        break;
      case CropDragMode::Move:
        box = drag_origin_box_.translated(cursor - drag_anchor_frame_);
        break;
      case CropDragMode::ResizeEdge:
        box = CropGeometry::ResizeBoxEdge(drag_origin_box_, drag_edge_, cursor,
                                          crop_state.aspect_locked, crop_state.aspect_ratio);
        break;
      case CropDragMode::ResizeCorner:
        box = CropGeometry::ResizeBoxFromFixedCorner(drag_fixed_corner_frame_, cursor,
                                                     crop_state.aspect_locked,
                                                     crop_state.aspect_ratio);
        break;
      case CropDragMode::None:
      case CropDragMode::RotateHandle:
        break;
    }
    new_rect = CropGeometry::CropFromFrameBox(box, drag_pre_press_rotation_, drag_source_);
  }

  crop_state.rect = new_rect;
  if (rotation_changed) {
    crop_state.rotation_degrees = new_rotation_degrees;
  }
  state.SetCropOverlayState(crop_state);
  drag_changed_          = true;
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
  result.consumed     = true;
  result.unset_cursor = true;
  if (drag_changed_) {
    const auto crop_state = state.GetCropOverlay();
    result.rect_changed   = crop_state.rect;
    result.rect_is_final  = true;
    if (drag_mode_ == CropDragMode::RotateHandle) {
      result.rotation_changed  = crop_state.rotation_degrees;
      result.rotation_is_final = true;
    }
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

  Cancel();
  crop_state.rotation_degrees = 0.0f;
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
  if (drag_mode_ != CropDragMode::None && drag_changed_) {
    auto crop_state             = state.GetCropOverlay();
    crop_state.rect             = drag_pre_press_rect_;
    crop_state.rotation_degrees = drag_pre_press_rotation_;
    state.SetCropOverlayState(crop_state);
  }
  Cancel();
}

void CropInteractionController::Cancel() {
  drag_mode_              = CropDragMode::None;
  drag_edge_              = CropEdge::None;
  drag_changed_           = false;
  drag_anchor_widget_pos_ = QPointF();
  drag_center_widget_pos_ = QPointF();
}

}  // namespace alcedo
