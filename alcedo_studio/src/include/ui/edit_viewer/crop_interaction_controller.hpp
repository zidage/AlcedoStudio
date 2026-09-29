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
 * ClampCropToRotatedSource. Rotation drags change the angle about the crop center; the center
 * stays on the same source content and the frame shrinks when it would leave the source.
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

 private:
  CropDragMode drag_mode_ = CropDragMode::None;
  CropEdge     drag_edge_ = CropEdge::None;
  Extent2D     drag_source_{};
  // Frame-space press point and crop box at press.
  QPointF      drag_anchor_frame_{};
  QRectF       drag_origin_box_{};
  QPointF      drag_fixed_corner_frame_{};
  // Widget-space press point and crop center for rotation drags.
  QPointF      drag_anchor_widget_pos_{};
  QPointF      drag_center_widget_pos_{};
  QRectF       drag_pre_press_rect_{0.0, 0.0, 1.0, 1.0};
  float        drag_pre_press_rotation_ = 0.0f;
  bool         drag_changed_            = false;
};

}  // namespace alcedo
