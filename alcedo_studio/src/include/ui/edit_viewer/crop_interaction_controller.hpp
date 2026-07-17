//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include <QPointF>
#include <QRectF>
#include <Qt>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

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
  bool                       consumed       = false;
  bool                       request_repaint = false;
  std::optional<Qt::CursorShape> cursor{};
  bool                       unset_cursor   = false;
  std::optional<QRectF>      rect_changed{};
  bool                       rect_is_final  = false;
  std::optional<float>       rotation_changed{};
  bool                       rotation_is_final = false;
};

struct CropPressContext {
  QPointF                    event_pos{};
  std::optional<QPointF>     image_uv{};
  CropHitTestResult          hit_test{};
  bool                       inside_image = false;
};

class CropInteractionController {
 public:
  CropInteractionController() = default;

  auto HandlePress(ViewerState& state, const ViewportWidgetInfo& widget_info,
                   const ViewportImageInfo& image_info, const QPointF& event_pos)
      -> CropInteractionResult;

  auto HandlePress(ViewerState& state, const ViewportImageInfo& image_info,
                   const CropPressContext& press_context)
      -> CropInteractionResult;

  auto HandleMove(ViewerState& state, const ViewportWidgetInfo& widget_info,
                  const ViewportImageInfo& image_info, Qt::MouseButtons buttons,
                  const QPointF& event_pos) -> CropInteractionResult;

  auto HandleRelease(ViewerState& state) -> CropInteractionResult;
  auto HandleDoubleClick(ViewerState& state) -> CropInteractionResult;
  // Aborts an in-flight drag without a final commit. Restores the crop rect and
  // rotation captured at press so a later release cannot apply a stale provisional
  // result after the tool is disabled or the session cancels.
  void Cancel(ViewerState& state);
  void Cancel();

 private:
  auto MakeRectEmissionResult(const QRectF& rect, bool is_final) const -> CropInteractionResult;

  CropDragMode drag_mode_               = CropDragMode::None;
  CropCorner   drag_corner_             = CropCorner::None;
  CropEdge     drag_edge_               = CropEdge::None;
  QPointF      drag_anchor_uv_{};
  QPointF      drag_anchor_widget_pos_{};
  QRectF       drag_origin_rect_{};
  // Crop rect present at press, before Create/Move provisional mutation. Cancel
  // restores this so disabling the tool mid-gesture never leaves a half-built rect.
  QRectF       drag_pre_press_rect_{0.0, 0.0, 1.0, 1.0};
  float        drag_pre_press_rotation_ = 0.0f;
  QPointF      drag_fixed_corner_uv_{};
  float        drag_rotation_degrees_   = 0.0f;
};

}  // namespace alcedo
