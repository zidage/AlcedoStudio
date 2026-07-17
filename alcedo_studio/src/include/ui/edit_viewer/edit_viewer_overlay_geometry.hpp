//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <optional>

#include <QPointF>
#include <QRectF>
#include <Qt>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {

struct EditViewerOverlaySnapshot {
  ViewerStateSnapshot   viewer_state{};
  ViewportWidgetInfo    widget_info{};
  ViewportImageInfo     image_info{};
  FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
  // Optional detail-patch / ROI bounds in source-image UV (0..1). When valid,
  // overlay drawing may show a guide rectangle independent of crop handles.
  bool                  detail_roi_visible = false;
  QRectF                detail_roi_uv      = QRectF(0.0, 0.0, 1.0, 1.0);
};

struct CropOverlayWidgetGeometry {
  QRectF                 image_rect{};
  bool                   image_rect_valid    = false;
  std::array<QPointF, 4> crop_corners_widget{};
  bool                   crop_corners_valid  = false;
  QPointF                rotate_stem_widget{};
  QPointF                rotate_handle_widget{};
  std::array<QPointF, 4> detail_roi_corners_widget{};
  bool                   detail_roi_valid    = false;
};

enum class EditViewerOverlayHitKind {
  None,
  OutsideImage,
  BlankInImage,
  InsideCrop,
  Edge,
  Corner,
  RotateHandle,
};

struct EditViewerOverlayHover {
  EditViewerOverlayHitKind kind         = EditViewerOverlayHitKind::None;
  CropHitTestResult        crop_hit{};
  std::optional<QPointF>   image_uv{};
  bool                     inside_image = false;
  std::optional<Qt::CursorShape> cursor{};
};

class EditViewerOverlayGeometry {
 public:
  static auto Build(const EditViewerOverlaySnapshot& snapshot) -> CropOverlayWidgetGeometry;

  static auto ComputeHover(const EditViewerOverlaySnapshot& snapshot,
                           const CropOverlayWidgetGeometry& geometry, const QPointF& event_pos)
      -> EditViewerOverlayHover;
};

}  // namespace alcedo
