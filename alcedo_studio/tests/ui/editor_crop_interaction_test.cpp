//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_crop_interaction_test.cpp
/// @brief Geometry crop frame on the editor viewport (#221): the frame maps
/// through the presented frame's geometry, the source size comes from that
/// frame, pointer edits run in frame space under the rotated preview and are
/// constrained to the rotated source, and the view stays at fit while the crop
/// tool is open.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QPointF>
#include <QRectF>
#include <QSignalSpy>
#include <QTest>
#include <Qt>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "edit/geometry/crop_frame.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/crop_interaction_controller.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"

namespace alcedo::editor_rhi {
namespace {

auto ExtentOf(int width, int height) -> Extent2D {
  return Extent2D{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

// Presents an uncropped, unrotated frame of a width x height source, as the
// session does when a frame reaches the viewport.
void ConfigureImage(EditorInteractionController& controller, int width = 4000, int height = 3000) {
  controller.setDisplayedMaskGeometry(
      MaskEditGeometry::MakeIdentityPhotographGeometry(ExtentOf(width, height)));
  controller.setRenderReferenceSize(width, height);
}

// Presents a frame resolved from document geometry, as the renderer produces it.
void PresentResolvedFrame(EditorInteractionController& controller, int width, int height,
                          const ImageGeometryParams& image) {
  const auto geometry = ResolveRenderGeometry(
      MakeSourceGeometry(ExtentOf(width, height), ExtentOf(width, height)), image, {}, {}, {});
  controller.setDisplayedMaskGeometry(geometry);
  controller.setRenderReferenceSize(static_cast<int>(geometry.render_extent.width),
                                    static_cast<int>(geometry.render_extent.height));
}

// Presents the Geometry panel frame: the whole source rotated by the document rotation.
void PresentGeometryPanelFrame(EditorInteractionController& controller, int width, int height,
                               float degrees) {
  ImageGeometryParams image;
  image.rotation_degrees = degrees;
  image.output_frame     = GeometryOutputFrame::RotatedSourceBounds;
  PresentResolvedFrame(controller, width, height, image);
}

void OpenCropTool(EditorInteractionController& controller) {
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
}

// Presented-frame mapping of an uncropped, unrotated image at fit.
auto MakeIdentityMapping(const ViewportWidgetInfo& widget, int width, int height)
    -> MaskEditViewMapping {
  MaskEditViewMapping mapping;
  mapping.widget     = widget;
  mapping.photograph = {width, height};
  mapping.geometry   = MaskEditGeometry::MakeIdentityPhotographGeometry(ExtentOf(width, height));
  return mapping;
}

// Opens the crop tool on an unrotated 400x300 Geometry panel frame in an 800x600 viewport
// with the crop at (0.2, 0.2, 0.6, 0.6): widget corners (160, 120) to (640, 480).
void OpenInsetCrop(EditorInteractionController& controller) {
  controller.setViewportMetrics(800, 600, 1.0);
  PresentGeometryPanelFrame(controller, 400, 300, 0.0f);
  OpenCropTool(controller);
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.6, 0.6));
}

void Drag(EditorInteractionController& controller, const QPointF& from,
          std::initializer_list<QPointF> moves) {
  controller.handlePress(from.x(), from.y(), static_cast<int>(Qt::LeftButton));
  QPointF last = from;
  for (const QPointF& move : moves) {
    controller.handleMove(move.x(), move.y(), static_cast<int>(Qt::LeftButton));
    last = move;
  }
  controller.handleRelease(last.x(), last.y(), static_cast<int>(Qt::LeftButton));
}

// The point at @p radius from @p center in the direction of @p degrees (item space, y-down).
auto PointOnCircle(const QPointF& center, double radius, double degrees) -> QPointF {
  const double radians = degrees * 3.14159265358979323846 / 180.0;
  return center + QPointF(radius * std::cos(radians), radius * std::sin(radians));
}

}  // namespace

TEST(EditorCropInteractionTest, HitTestSeparatesEdgeBandsFromCornerRotateZones) {
  const std::array<QPointF, 4> square{QPointF(100, 100), QPointF(300, 100), QPointF(300, 300),
                                      QPointF(100, 300)};
  const auto                   hit = [&](double x, double y) {
    return CropGeometry::HitTestWidgetGeometry(square, QPointF(x, y));
  };

  EXPECT_EQ(hit(100, 100).corner_index, 0);
  // The top edge band reaches 18 px outside and 10 px inside.
  EXPECT_EQ(hit(200, 84).edge, CropEdge::Top);
  EXPECT_EQ(hit(200, 108).edge, CropEdge::Top);
  EXPECT_FALSE(hit(200, 84).rotate_hit);
  // Above the middle of an edge there is no rotate zone (the old handle spot is 28 px out).
  EXPECT_EQ(hit(200, 72).edge, CropEdge::None);
  EXPECT_FALSE(hit(200, 72).rotate_hit);
  EXPECT_FALSE(hit(200, 40).rotate_hit);
  // Outside a corner, and beside a corner past the edge band, is a rotate zone.
  EXPECT_TRUE(hit(320, 80).rotate_hit);
  EXPECT_TRUE(hit(330, 110).rotate_hit);
  EXPECT_FALSE(hit(400, 0).rotate_hit);
  // Inside the frame there are no frame controls.
  const auto inside = hit(200, 200);
  EXPECT_EQ(inside.corner_index, -1);
  EXPECT_EQ(inside.edge, CropEdge::None);
  EXPECT_FALSE(inside.rotate_hit);
}

TEST(EditorCropInteractionTest, DragFromAboveTheTopEdgeResizesAndNeverRotates) {
  // #942: a drag that starts a little above the top edge grip must move the top edge.
  EditorInteractionController controller;
  OpenInsetCrop(controller);
  const QPointF top_mid(400.0, 120.0);

  Drag(controller, top_mid + QPointF(0.0, -16.0),
       {top_mid + QPointF(1.0, 20.0), top_mid + QPointF(-2.0, 80.0)});

  EXPECT_FLOAT_EQ(controller.cropRotationDegrees(), 0.0f);
  const QRectF rect = controller.cropRectNormalized();
  // 96 item px down at 2 item px per source px is 48 source px, 0.16 of the height.
  EXPECT_NEAR(rect.top(), 0.36, 2e-3);
  EXPECT_NEAR(rect.bottom(), 0.8, 1e-4);
  EXPECT_NEAR(rect.left(), 0.2, 1e-4);
  EXPECT_NEAR(rect.width(), 0.6, 1e-4);

  // At the old rotate handle position (28 px out) the drag does not rotate either.
  OpenInsetCrop(controller);
  Drag(controller, QPointF(400.0, 92.0), {QPointF(402.0, 150.0), QPointF(398.0, 200.0)});
  EXPECT_FLOAT_EQ(controller.cropRotationDegrees(), 0.0f);
}

TEST(EditorCropInteractionTest, RotationDragHoldsNearTheCenterAndNeverFlips) {
  EditorInteractionController controller;
  OpenInsetCrop(controller);
  const QPointF center(400.0, 300.0);
  const QPointF zone(670.0, 90.0);

  controller.handlePress(zone.x(), zone.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(500.0, 200.0, static_cast<int>(Qt::LeftButton));
  const float after_first = controller.cropRotationDegrees();
  EXPECT_GT(std::abs(after_first), 1.0f);
  // Into the center and out on the other side: the old handle math turned this into
  // about 180 degrees. Near the center the angle holds; leaving it re-anchors.
  controller.handleMove(center.x() + 10.0, center.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(center.x() - 100.0, center.y() + 120.0, static_cast<int>(Qt::LeftButton));
  controller.handleRelease(center.x() - 100.0, center.y() + 120.0,
                           static_cast<int>(Qt::LeftButton));

  EXPECT_NEAR(controller.cropRotationDegrees(), after_first, 1e-3f);
}

TEST(EditorCropInteractionTest, RotationDragIsLimitedTo45DegreesPerDrag) {
  EditorInteractionController controller;
  OpenInsetCrop(controller);
  const QPointF center(400.0, 300.0);
  const double  radius = 342.0;
  const double  start  = -37.87;

  controller.handlePress(PointOnCircle(center, radius, start).x(),
                         PointOnCircle(center, radius, start).y(),
                         static_cast<int>(Qt::LeftButton));
  // Swing 120 degrees in 10-degree steps.
  for (int step = 1; step <= 12; ++step) {
    const QPointF p = PointOnCircle(center, radius, start - (10.0 * step));
    controller.handleMove(p.x(), p.y(), static_cast<int>(Qt::LeftButton));
  }
  EXPECT_NEAR(std::abs(controller.cropRotationDegrees()), CropGeometry::kCropRotateMaxDragDegrees,
              1e-3f);
  // Turning back reduces the angle at once.
  const QPointF back = PointOnCircle(center, radius, start - 110.0);
  controller.handleMove(back.x(), back.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_NEAR(std::abs(controller.cropRotationDegrees()),
              CropGeometry::kCropRotateMaxDragDegrees - 10.0f, 0.05f);
  controller.handleRelease(back.x(), back.y(), static_cast<int>(Qt::LeftButton));
}

TEST(EditorCropInteractionTest, RotationDragStartsAfterTravelSnapsToZeroAndShowsTheFineGrid) {
  EditorInteractionController controller;
  OpenInsetCrop(controller);
  const QPointF center(400.0, 300.0);
  const QPointF zone(670.0, 90.0);
  const QPointF radial = (zone - center) / std::hypot(zone.x() - center.x(), zone.y() - center.y());
  const QPointF perpendicular(-radial.y(), radial.x());
  QSignalSpy    edited(&controller, &EditorInteractionController::cropFrameEdited);

  controller.handlePress(zone.x(), zone.y(), static_cast<int>(Qt::LeftButton));
  // Less than the start travel: nothing changes and no grid.
  const QPointF small = zone + (perpendicular * 2.0);
  controller.handleMove(small.x(), small.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_EQ(edited.count(), 0);
  EXPECT_FALSE(controller.overlayGeometry().crop_rotation_active);

  // About 0.2 degrees: the drag starts, the angle snaps to 0, and the fine grid is drawn.
  const QPointF nearly_level = zone + (radial * 4.0) + (perpendicular * 1.2);
  controller.handleMove(nearly_level.x(), nearly_level.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_FLOAT_EQ(controller.cropRotationDegrees(), 0.0f);
  const auto rotating = controller.overlayGeometry();
  EXPECT_TRUE(rotating.crop_rotation_active);
  EXPECT_FALSE(BuildOverlaySceneGeometry(rotating, true).fine_grid_triangles.empty());

  const QPointF turned = zone + (perpendicular * 30.0);
  controller.handleMove(turned.x(), turned.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_GT(std::abs(controller.cropRotationDegrees()), 1.0f);

  controller.handleRelease(turned.x(), turned.y(), static_cast<int>(Qt::LeftButton));
  const auto released = controller.overlayGeometry();
  EXPECT_FALSE(released.crop_rotation_active);
  EXPECT_TRUE(BuildOverlaySceneGeometry(released, true).fine_grid_triangles.empty());
}

TEST(EditorCropInteractionTest, AspectLockedBoxPreservesPixelRatio) {
  const QRectF box = CropGeometry::MakeAspectLockedBoxFromDiagonal(QPointF(80.0, 60.0),
                                                                   QPointF(280.0, 150.0),
                                                                   16.0f / 9.0f);
  ASSERT_GT(box.width(), 0.0);
  ASSERT_GT(box.height(), 0.0);
  EXPECT_NEAR(box.width() / box.height(), 16.0 / 9.0, 1e-4);
  EXPECT_EQ(box.topLeft(), QPointF(80.0, 60.0));
}

TEST(EditorCropInteractionTest, FrameBoxRoundTripsThroughTheDocumentCrop) {
  const Extent2D source{400, 300};
  const QRectF   crop = CropGeometry::ClampCrop(QRectF(0.2, 0.3, 0.4, 0.3), 23.0f, source);
  const QRectF   box  = CropGeometry::FrameBoxFromCrop(crop, 23.0f, source);
  // The frame box has the crop's size in source pixels.
  EXPECT_NEAR(box.width(), crop.width() * 400.0, 1e-3);
  EXPECT_NEAR(box.height(), crop.height() * 300.0, 1e-3);
  const QRectF back = CropGeometry::CropFromFrameBox(box, 23.0f, source);
  EXPECT_NEAR(back.x(), crop.x(), 1e-5);
  EXPECT_NEAR(back.y(), crop.y(), 1e-5);
  EXPECT_NEAR(back.width(), crop.width(), 1e-5);
  EXPECT_NEAR(back.height(), crop.height(), 1e-5);
}

TEST(EditorCropInteractionTest, CreateDragReplacesTheCropOnlyAfterThePointerMoves) {
  ViewerState               state;
  CropInteractionController controller;
  auto                      crop_state = state.GetCropOverlay();
  crop_state.tool_enabled              = true;
  crop_state.overlay_visible           = true;
  crop_state.rect                      = QRectF(0.25, 0.25, 0.5, 0.5);
  state.SetCropOverlayState(crop_state);
  const ViewportWidgetInfo widget{800, 600, 1.0f};
  const auto               mapping = MakeIdentityMapping(widget, 400, 300);

  // 400x300 fills the 800x600 widget at scale 2.
  const auto press = controller.HandlePress(state, mapping, QPointF(80.0, 60.0));
  EXPECT_TRUE(press.consumed);
  EXPECT_FALSE(press.rect_changed.has_value());

  const auto move = controller.HandleMove(state, mapping, Qt::LeftButton, QPointF(320.0, 270.0));
  ASSERT_TRUE(move.rect_changed.has_value());
  EXPECT_FALSE(move.rect_is_final);
  EXPECT_NEAR(move.rect_changed->x(), 0.1, 1e-4);
  EXPECT_NEAR(move.rect_changed->y(), 0.1, 1e-4);
  EXPECT_NEAR(move.rect_changed->width(), 0.3, 1e-4);
  EXPECT_NEAR(move.rect_changed->height(), 0.35, 1e-4);

  const auto release = controller.HandleRelease(state);
  EXPECT_TRUE(release.consumed);
  ASSERT_TRUE(release.rect_changed.has_value());
  EXPECT_TRUE(release.rect_is_final);

  // A click without movement leaves the crop alone and reports nothing.
  controller.HandlePress(state, mapping, QPointF(700.0, 560.0));
  const auto click = controller.HandleRelease(state);
  EXPECT_TRUE(click.consumed);
  EXPECT_FALSE(click.rect_changed.has_value());
}

TEST(EditorCropInteractionTest, ClampCropRectKeepsRotatedCornersInsidePresentedSource) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  const QRectF near_full(0.05, 0.05, 0.9, 0.9);
  // Before a frame is presented the source size is unknown; the rect is returned as given.
  EXPECT_EQ(controller.clampCropRect(near_full, 35.0f), near_full);

  ConfigureImage(controller, 400, 300);
  const QRectF clamped = controller.clampCropRect(near_full, 35.0f);
  EXPECT_LT(clamped.width(), near_full.width());
  for (const auto& corner : CropFrameCornersInReference(CropGeometry::ToNormalizedRect(clamped),
                                                        35.0f, Extent2D{400, 300})) {
    EXPECT_GE(corner.x, -1e-2);
    EXPECT_LE(corner.x, 400.0 + 1e-2);
    EXPECT_GE(corner.y, -1e-2);
    EXPECT_LE(corner.y, 300.0 + 1e-2);
  }
}

TEST(EditorCropInteractionTest, CropSourceSizeComesFromPresentedReferenceExtentAfterImageSwitch) {
  // Regression for #221: the Geometry panel's source size must be the presented
  // frame's full reference extent, never a frame or viewport size.
  EditorInteractionController controller;
  controller.setViewportMetrics(1200, 800, 1.0);
  QSignalSpy geometry_spy(&controller, &EditorInteractionController::imageGeometryChanged);

  // Image A is shown cropped: the output is 2400x1600, the source 6000x4000.
  ImageGeometryParams cropped;
  cropped.crop_rect = NormalizedRect{0.3f, 0.3f, 0.4f, 0.4f};
  PresentResolvedFrame(controller, 6000, 4000, cropped);
  EXPECT_EQ(controller.sourceImageWidth(), 6000);
  EXPECT_EQ(controller.sourceImageHeight(), 4000);
  EXPECT_GE(geometry_spy.count(), 1);

  // A render reference of another size (a detail or viewport-sized frame) does not change it.
  controller.setRenderReferenceSize(1200, 800);
  EXPECT_EQ(controller.sourceImageWidth(), 6000);

  // Switching images clears the size until image B's first frame arrives.
  controller.resetPresentationStateForNewImage();
  EXPECT_EQ(controller.sourceImageWidth(), 0);
  EXPECT_EQ(controller.sourceImageHeight(), 0);
  ConfigureImage(controller, 3000, 4500);
  EXPECT_EQ(controller.sourceImageWidth(), 3000);
  EXPECT_EQ(controller.sourceImageHeight(), 4500);

  // Back to image A: its crop is shown again against its own source size.
  controller.resetPresentationStateForNewImage();
  PresentResolvedFrame(controller, 6000, 4000, cropped);
  EXPECT_EQ(controller.sourceImageWidth(), 6000);
  EXPECT_EQ(controller.sourceImageHeight(), 4000);
}

TEST(EditorCropInteractionTest, CropFrameIsAxisAlignedOnTheRotatedGeometryPanelFrame) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  PresentGeometryPanelFrame(controller, 400, 300, 10.0f);
  OpenCropTool(controller);
  controller.setCropRotationDegrees(10.0f);
  controller.setCropRectNormalized(controller.clampCropRect(QRectF(0.3, 0.3, 0.4, 0.4), 10.0f));

  const auto geometry = controller.overlayGeometry();
  ASSERT_TRUE(geometry.crop_corners_valid);
  const auto& c = geometry.crop_corners_widget;
  // The source is rotated under the frame, so the frame's edges are screen-aligned.
  EXPECT_NEAR(c[0].y(), c[1].y(), 0.05);
  EXPECT_NEAR(c[1].x(), c[2].x(), 0.05);
  EXPECT_NEAR(c[2].y(), c[3].y(), 0.05);
  EXPECT_NEAR(c[3].x(), c[0].x(), 0.05);
  EXPECT_LT(c[0].x(), c[1].x());
  EXPECT_LT(c[0].y(), c[3].y());
}

TEST(EditorCropInteractionTest, CropEdgeDragUnderRotationMovesOnlyThatFrameEdge) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  PresentGeometryPanelFrame(controller, 400, 300, 10.0f);
  OpenCropTool(controller);
  controller.setCropRotationDegrees(10.0f);
  const QRectF initial = controller.clampCropRect(QRectF(0.3, 0.3, 0.3, 0.3), 10.0f);
  controller.setCropRectNormalized(initial);
  const QRectF box_before = CropGeometry::FrameBoxFromCrop(initial, 10.0f, Extent2D{400, 300});

  const auto   corners    = controller.overlayGeometry().crop_corners_widget;
  const QPointF right_mid = CropGeometry::LerpPoint(corners[1], corners[2], 0.5f);
  QSignalSpy   edited(&controller, &EditorInteractionController::cropFrameEdited);
  controller.handlePress(right_mid.x(), right_mid.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(right_mid.x() + 30.0, right_mid.y(), static_cast<int>(Qt::LeftButton));
  controller.handleRelease(right_mid.x() + 30.0, right_mid.y(), static_cast<int>(Qt::LeftButton));

  ASSERT_EQ(edited.count(), 2);
  EXPECT_FALSE(edited.at(0).at(2).toBool());
  EXPECT_TRUE(edited.at(1).at(2).toBool());
  EXPECT_NEAR(edited.at(1).at(1).toFloat(), 10.0f, 1.0e-4f);
  const QRectF box_after = CropGeometry::FrameBoxFromCrop(controller.cropRectNormalized(), 10.0f,
                                                          Extent2D{400, 300});
  // In frame space the left, top, and bottom edges stay; the right edge moves outward.
  EXPECT_NEAR(box_after.left(), box_before.left(), 0.05);
  EXPECT_NEAR(box_after.top(), box_before.top(), 0.05);
  EXPECT_NEAR(box_after.bottom(), box_before.bottom(), 0.05);
  EXPECT_GT(box_after.right(), box_before.right() + 5.0);
}

TEST(EditorCropInteractionTest, RotationDragShrinksFrameToStayInsideSource) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  PresentGeometryPanelFrame(controller, 400, 300, 0.0f);
  OpenCropTool(controller);
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.6, 0.6));

  const auto    corners = controller.overlayGeometry().crop_corners_widget;
  const auto    center  = CropGeometry::CropCenterWidgetPoint(corners);
  // Press in the rotate zone outside the top-right corner and swing it about the center.
  const QPointF zone    = corners[1] + QPointF(30.0, -30.0);
  const QPointF target  = center + QPointF(zone.x() - center.x() + 60.0, zone.y() - center.y());
  QSignalSpy    edited(&controller, &EditorInteractionController::cropFrameEdited);
  controller.handlePress(zone.x(), zone.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(target.x(), target.y(), static_cast<int>(Qt::LeftButton));
  controller.handleRelease(target.x(), target.y(), static_cast<int>(Qt::LeftButton));

  ASSERT_GE(edited.count(), 2);
  EXPECT_TRUE(edited.last().at(2).toBool());
  const float degrees = controller.cropRotationDegrees();
  EXPECT_GT(std::abs(degrees), 1.0f);
  const QRectF rect = controller.cropRectNormalized();
  EXPECT_LT(rect.width(), 1.0);
  // The frame center stays on the source center; every rotated corner is inside the source.
  EXPECT_NEAR(rect.center().x(), 0.5, 1.0e-4);
  EXPECT_NEAR(rect.center().y(), 0.5, 1.0e-4);
  for (const auto& corner : CropFrameCornersInReference(CropGeometry::ToNormalizedRect(rect),
                                                        degrees, Extent2D{400, 300})) {
    EXPECT_GE(corner.x, -1e-2);
    EXPECT_LE(corner.x, 400.0 + 1e-2);
    EXPECT_GE(corner.y, -1e-2);
    EXPECT_LE(corner.y, 300.0 + 1e-2);
  }
}

TEST(EditorCropInteractionTest, CropToolLocksTheViewToFit) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);
  controller.applyViewTransformForTest(3.0f, 0.1f, 0.0f);

  // Entering the crop tool from a zoomed ROI view returns to fit.
  OpenCropTool(controller);
  EXPECT_NEAR(controller.zoom(), 1.0f, 1.0e-4f);
  EXPECT_NEAR(controller.panX(), 0.0f, 1.0e-4f);

  QSignalSpy change_spy(&controller, &EditorInteractionController::viewChangeReported);
  controller.handleWheel(400, 300, 120, 0, 0, static_cast<int>(Qt::ControlModifier), false);
  controller.handlePinchTo(400, 300, 3.0);
  controller.zoomToActualPixels();
  controller.handleDoubleTap(10, 10);
  controller.handlePress(400, 300, static_cast<int>(Qt::MiddleButton));
  controller.handleMove(450, 330, static_cast<int>(Qt::MiddleButton));
  controller.handleRelease(450, 330, static_cast<int>(Qt::MiddleButton));
  QTest::qWait(200);
  QCoreApplication::processEvents();

  EXPECT_NEAR(controller.zoom(), 1.0f, 1.0e-4f);
  EXPECT_NEAR(controller.panX(), 0.0f, 1.0e-4f);
  EXPECT_NEAR(controller.panY(), 0.0f, 1.0e-4f);
  for (const auto& call : change_spy) {
    EXPECT_NE(call.at(0).toInt(),
              static_cast<int>(EditorInteractionController::ViewChangeKind::DetailRefresh));
  }
}

TEST(EditorCropInteractionTest, CropFrameEditedReportsPointerEditsOnlyAndNeverRoutesViewChanges) {
  // The Geometry panel submits crop edits through the document path. The
  // overlay reports pointer edits with cropFrameEdited and never asks the
  // session for a render itself.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  OpenCropTool(controller);

  QSignalSpy spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(spy.isValid());
  QSignalSpy edited(&controller, &EditorInteractionController::cropFrameEdited);
  ASSERT_TRUE(edited.isValid());

  // Showing document values (undo, image switch) does not report an edit.
  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));
  controller.setCropRotationDegrees(12.0f);
  EXPECT_NEAR(controller.cropRectNormalized().x(), 0.25, 1e-4);
  EXPECT_NEAR(controller.cropRotationDegrees(), 12.0f, 1e-4f);
  EXPECT_EQ(edited.count(), 0);
  EXPECT_TRUE(spy.empty());

  // A press alone changes nothing.
  const QPointF start = controller.imageUvToItemPoint(0.05, 0.05);
  const QPointF mid   = controller.imageUvToItemPoint(0.2, 0.2);
  const QPointF end   = controller.imageUvToItemPoint(0.3, 0.3);
  controller.handlePress(start.x(), start.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_EQ(edited.count(), 0);

  // Moves report non-final edits; the release reports one final edit.
  controller.handleMove(mid.x(), mid.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  controller.handleRelease(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  ASSERT_EQ(edited.count(), 3);
  EXPECT_FALSE(edited.at(0).at(2).toBool());
  EXPECT_FALSE(edited.at(1).at(2).toBool());
  EXPECT_TRUE(edited.at(2).at(2).toBool());
  EXPECT_TRUE(spy.empty()) << "crop-frame drag must not route pipeline view changes";
}

TEST(EditorCropInteractionTest, TrueZoomFollowsThePresentedFrameExtent) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);  // source 4000x3000, fitFraction 0.2
  EXPECT_NEAR(controller.trueZoom(), 0.2f, 1.0e-5f);

  // A crop overlay value alone does not change what is displayed.
  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));
  EXPECT_NEAR(controller.trueZoom(), 0.2f, 1.0e-5f);

  // The presented cropped output is 2000x1500 source pixels, so fit is 40%.
  ImageGeometryParams cropped;
  cropped.crop_rect = NormalizedRect{0.25f, 0.25f, 0.5f, 0.5f};
  PresentResolvedFrame(controller, 4000, 3000, cropped);
  EXPECT_NEAR(controller.trueZoom(), 0.4f, 1.0e-5f);

  // 1:1 is now 1 cropped-output pixel per screen pixel → field 1/0.4 = 2.5.
  controller.zoomToActualPixels();
  EXPECT_NEAR(controller.zoom(), 2.5f, 1.0e-4f);
  EXPECT_NEAR(controller.trueZoom(), 1.0f, 1.0e-4f);
}

}  // namespace alcedo::editor_rhi
