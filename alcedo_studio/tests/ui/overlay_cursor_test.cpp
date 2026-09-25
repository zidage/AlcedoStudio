//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCursor>
#include <QPointF>
#include <QRectF>
#include <array>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/overlay_cursor.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"

namespace alcedo {
namespace {

constexpr int   kImageWidth  = 400;
constexpr int   kImageHeight = 300;
const QRectF    kCropRect(0.25, 0.25, 0.5, 0.5);

void ConfigureCrop(editor_rhi::EditorInteractionController& controller, float rotation_degrees) {
  controller.setInteractionEnabled(true);
  controller.setViewportMetrics(800.0, 600.0, 1.0);
  controller.setImageSize(kImageWidth, kImageHeight);
  controller.setRenderReferenceSize(kImageWidth, kImageHeight);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(kCropRect);
  controller.setCropRotationDegrees(rotation_degrees);
}

[[nodiscard]] auto CropCornersItem(const editor_rhi::EditorInteractionController& controller,
                                   float rotation_degrees) -> std::array<QPointF, 4> {
  const auto corners_uv = CropGeometry::RotatedCropCornersUv(
      kCropRect, rotation_degrees, CropGeometry::SafeAspect(kImageWidth, kImageHeight));
  std::array<QPointF, 4> corners{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    corners[i] = controller.imageUvToItemPoint(corners_uv[i].x(), corners_uv[i].y());
  }
  return corners;
}

}  // namespace

TEST(OverlayCursorTest, ResizeAxisQuantizesToFourResizeCursors) {
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(1.0, 0.1)), OverlayCursor::ResizeHorizontal);
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(-1.0, 0.0)), OverlayCursor::ResizeHorizontal);
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(0.0, -1.0)), OverlayCursor::ResizeVertical);
  // Item space is y-down: (+x, +y) is toward the bottom-right.
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(1.0, 1.0)), OverlayCursor::ResizeDiagonalDown);
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(-1.0, -1.0)), OverlayCursor::ResizeDiagonalDown);
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF(1.0, -1.0)), OverlayCursor::ResizeDiagonalUp);
  EXPECT_EQ(OverlayResizeCursorForAxis(QPointF()), OverlayCursor::None);

  EXPECT_EQ(OverlayCursorShape(OverlayCursor::Move), Qt::SizeAllCursor);
  EXPECT_EQ(OverlayCursorShape(OverlayCursor::Rotate), kOverlayRotateCursorShape);
  EXPECT_FALSE(OverlayCursorShape(OverlayCursor::None).has_value());
  EXPECT_FALSE(OverlayRotateCursor().pixmap().isNull());
}

TEST(OverlayCursorTest, CropHitCursorFollowsRotatedCornersAndEdges) {
  // Unrotated crop: corners are diagonal, edges resize across their side.
  std::array<QPointF, 4> square{QPointF(100, 100), QPointF(300, 100), QPointF(300, 300),
                                QPointF(100, 300)};
  auto hit = CropGeometry::HitTestWidgetGeometry(square, square[0]);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeDiagonalDown);
  hit = CropGeometry::HitTestWidgetGeometry(square, square[1]);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeDiagonalUp);
  hit = CropGeometry::HitTestWidgetGeometry(square, QPointF(200, 100));
  ASSERT_EQ(hit.edge, CropEdge::Top);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeVertical);
  hit = CropGeometry::HitTestWidgetGeometry(square, QPointF(300, 200));
  ASSERT_EQ(hit.edge, CropEdge::Right);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeHorizontal);

  // Rotated 45 degrees clockwise: the top-left corner now points straight up
  // from the center, and the top edge faces the top-right.
  std::array<QPointF, 4> diamond{QPointF(200, 60), QPointF(340, 200), QPointF(200, 340),
                                 QPointF(60, 200)};
  hit = CropGeometry::HitTestWidgetGeometry(diamond, diamond[0]);
  ASSERT_EQ(hit.corner_index, 0);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeVertical);
  hit = CropGeometry::HitTestWidgetGeometry(diamond, QPointF(270, 130));
  ASSERT_EQ(hit.edge, CropEdge::Top);
  EXPECT_EQ(CropGeometry::CursorForCropHit(hit), OverlayCursor::ResizeDiagonalUp);

  CropHitTestResult rotate;
  rotate.rotate_handle_hit = true;
  EXPECT_EQ(CropGeometry::CursorForCropHit(rotate), OverlayCursor::Rotate);
  CropHitTestResult inside;
  inside.inside_crop = true;
  EXPECT_EQ(CropGeometry::CursorForCropHit(inside), OverlayCursor::Move);
}

TEST(OverlayCursorTest, CropRotateHandleShowsBitmapCursorOnOverlayItem) {
  editor_rhi::EditorInteractionController controller;
  ConfigureCrop(controller, 0.0f);
  editor_rhi::EditorOverlayItem overlay;
  overlay.setInteraction(&controller);

  const QPointF handle = controller.rotateHandleItemPos();
  controller.handleHoverMove(handle.x(), handle.y());
  ASSERT_TRUE(controller.hasCustomCursor());
  EXPECT_EQ(controller.cursorShape(), static_cast<int>(kOverlayRotateCursorShape));
  EXPECT_EQ(overlay.cursor().shape(), Qt::BitmapCursor);
  EXPECT_FALSE(overlay.cursor().pixmap().isNull());

  // Crop move and resize cursors stay with the viewport HoverHandler.
  const auto corners = CropCornersItem(controller, 0.0f);
  controller.handleHoverMove(corners[0].x(), corners[0].y());
  EXPECT_EQ(controller.cursorShape(), static_cast<int>(Qt::SizeFDiagCursor));
  EXPECT_EQ(overlay.cursor().shape(), Qt::ArrowCursor);
  const QPointF center = (corners[0] + corners[2]) * 0.5;
  controller.handleHoverMove(center.x(), center.y());
  EXPECT_EQ(controller.cursorShape(), static_cast<int>(Qt::SizeAllCursor));
}

TEST(OverlayCursorTest, RotatedCropCornerHoverFollowsScreenAxis) {
  editor_rhi::EditorInteractionController controller;
  // 45 degrees turns every corner onto a horizontal or vertical axis.
  ConfigureCrop(controller, 45.0f);
  const auto corners = CropCornersItem(controller, 45.0f);
  const QPointF center = (corners[0] + corners[2]) * 0.5;
  for (const auto& corner : corners) {
    controller.handleHoverMove(corner.x(), corner.y());
    ASSERT_TRUE(controller.hasCustomCursor());
    const QPointF axis = corner - center;
    const int expected = static_cast<int>(*OverlayCursorShape(OverlayResizeCursorForAxis(axis)));
    EXPECT_EQ(controller.cursorShape(), expected);
    EXPECT_TRUE(controller.cursorShape() == static_cast<int>(Qt::SizeHorCursor) ||
                controller.cursorShape() == static_cast<int>(Qt::SizeVerCursor));
  }
}

}  // namespace alcedo
