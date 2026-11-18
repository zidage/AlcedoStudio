//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include <QPointF>
#include <QRectF>
#include <Qt>

#include "edit/geometry/types.hpp"
#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/viewer_state.hpp"

namespace alcedo {

enum class CropDragMode {
  None,
  Create,
  Move,
  ResizeEdge,
  ResizeCorner,
  RotateHandle,
};

struct CropInteractionResult {
  bool                           consumed        = false;
  bool                           request_repaint = false;
  std::optional<Qt::CursorShape> cursor{};
  bool                           unset_cursor    = false;
  std::optional<QRectF>          rect_changed{};
  bool                           rect_is_final   = false;
  std::optional<float>           rotation_changed{};
  bool                           rotation_is_final = false;
};

/**
 * @brief Pointer edits of the Geometry crop frame.
 *
 * Pointer positions map to source reference pixels through @p mapping, the presented frame's
 * geometry (the same mapping Mask input uses). While the Geometry panel is open that frame is the
 * whole source rotated by the document rotation, so the crop frame is axis-aligned on screen.
 * Edits are done in frame space (see CropGeometry) and every result is constrained with
 * ClampCropToRotatedSource. Rotation drags start in the rotate zones outside the frame corners
 * and change the angle about the crop center; the center stays on the same source content and
 * the frame shrinks when it would leave the source. A rotation drag starts after
 * CropGeometry::kCropRotateStartPx of travel, holds its angle while the pointer is near the
 * center, changes the angle by at most CropGeometry::kCropRotateMaxDragDegrees, and snaps to
 * 0 degrees.
 *
 * A press alone never changes the crop. Moves report non-final changes and the release reports
 * the final value only when the drag changed something.
 */
class CropInteractionController {
 public:
  CropInteractionController() = default;

  /// Widget-space crop corners, clockwise from the output's top-left. Empty when @p mapping
  /// cannot map the crop (no presented frame yet).
  [[nodiscard]] static auto CropCornersWidget(const CropOverlayState&    crop,
                                              const MaskEditViewMapping& mapping)
      -> std::optional<std::array<QPointF, 4>>;

  auto HandlePress(ViewerState& state, const MaskEditViewMapping& mapping,
                   const QPointF& event_pos) -> CropInteractionResult;

  auto HandleMove(ViewerState& state, const MaskEditViewMapping& mapping, Qt::MouseButtons buttons,
                  const QPointF& event_pos) -> CropInteractionResult;

  auto HandleRelease(ViewerState& state) -> CropInteractionResult;
  auto HandleDoubleClick(ViewerState& state) -> CropInteractionResult;
  // Aborts an in-flight drag without a final commit. Restores the crop rect and
  // rotation captured at press so a later release cannot apply a stale provisional
  // result after the tool is disabled or the session cancels.
  void Cancel(ViewerState& state);
  void Cancel();

  /// True while a rotate drag has started to change the angle (the overlay draws the fine grid).
  [[nodiscard]] auto RotationActive() const -> bool {
    return drag_mode_ == CropDragMode::RotateHandle && drag_rotation_started_;
  }

 private:
  CropDragMode drag_mode_ = CropDragMode::None;
  CropEdge     drag_edge_ = CropEdge::None;
  Extent2D     drag_source_{};
  // Frame-space press point and crop box at press.
  QPointF      drag_anchor_frame_{};
  QRectF       drag_origin_box_{};
  QPointF      drag_fixed_corner_frame_{};
  // Frame-space press point minus the grabbed edge or corner, so a press in the edge band
  // keeps its distance from the edge instead of snapping the edge to the pointer.
  QPointF      drag_grab_offset_frame_{};
  // Widget-space press point and crop center for rotation drags.
  QPointF      drag_anchor_widget_pos_{};
  QPointF      drag_center_widget_pos_{};
  // Rotation drag: the angle change is the sum of per-move pointer angle steps about the
  // center, so it never wraps. Moves near the center hold the angle and re-anchor the step.
  bool         drag_rotation_started_   = false;
  bool         drag_rotation_reanchor_  = false;
  float        drag_last_pointer_angle_ = 0.0f;
  float        drag_rotation_delta_     = 0.0f;
  QRectF       drag_pre_press_rect_{0.0, 0.0, 1.0, 1.0};
  float        drag_pre_press_rotation_ = 0.0f;
  bool         drag_changed_            = false;
};

}  // namespace alcedo
