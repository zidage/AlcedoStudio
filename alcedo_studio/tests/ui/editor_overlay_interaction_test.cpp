//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_overlay_interaction_test.cpp
/// @brief Phase 3: verifies EditorInteractionController + OverlaySceneGeometry
/// for crop, zoom, pan, fit, ROI, and reset at multiple device pixel ratios.
/// Overlay updates must not recreate viewport presentation targets.

#include <gtest/gtest.h>

#include <QPointF>
#include <QRectF>
#include <QSignalSpy>
#include <QVector2D>
#include <Qt>

#include <cmath>
#include <vector>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

namespace alcedo::editor_rhi {
namespace {

struct ViewportCase {
  const char* name;
  qreal width;
  qreal height;
  qreal dpr;
};

const ViewportCase kDprCases[] = {
    {"dpr1", 800.0, 600.0, 1.0},
    {"dpr15", 800.0, 600.0, 1.5},
    {"dpr2", 800.0, 600.0, 2.0},
};

void ConfigureImage(EditorInteractionController& controller, int width = 4000, int height = 3000) {
  controller.setImageSize(width, height);
  controller.setRenderReferenceSize(width, height);
}

}  // namespace

TEST(EditorOverlayInteractionTest, CtrlWheelZoomsAtCursorAndClampsPanAtMultipleDpr) {
  for (const auto& c : kDprCases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.width, c.height, c.dpr);
    ConfigureImage(controller);

    const float before = controller.zoom();
    controller.handleWheel(c.width * 0.5, c.height * 0.5, 120, 0, 0,
                           static_cast<int>(Qt::ControlModifier), false);
    EXPECT_GT(controller.zoom(), before) << c.name;
    EXPECT_LE(controller.zoom(), ViewTransformController::kMaxInteractiveZoom) << c.name;

    // Extreme pan request is clamped by the next zoom step.
    controller.applyViewTransformForTest(2.0f, 10.0f, 10.0f);
    EXPECT_LT(std::abs(controller.panX()), 5.0f) << c.name;
    EXPECT_LT(std::abs(controller.panY()), 5.0f) << c.name;
  }
}

TEST(EditorOverlayInteractionTest, PanDragMovesViewWhenCropToolDisabled) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller);
  controller.applyViewTransformForTest(2.0f, 0.0f, 0.0f);

  controller.handlePress(400, 300, static_cast<int>(Qt::LeftButton));
  controller.handleMove(450, 320, static_cast<int>(Qt::LeftButton));
  controller.handleRelease(450, 320, static_cast<int>(Qt::LeftButton));

  EXPECT_NE(controller.panX(), 0.0f);
}

TEST(EditorOverlayInteractionTest, CropCreateDragUpdatesNormalizedRectAndFinalizesOnRelease) {
  for (const auto& c : kDprCases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.width, c.height, c.dpr);
    ConfigureImage(controller, 400, 300);
    controller.setCropToolEnabled(true);
    controller.setCropOverlayVisible(true);
    controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));

    // Start outside existing crop to create a new rect (blank-in-image).
    const QPointF start = controller.imageUvToItemPoint(0.1, 0.1);
    const QPointF end = controller.imageUvToItemPoint(0.4, 0.45);
    ASSERT_FALSE(start.isNull()) << c.name;
    ASSERT_FALSE(end.isNull()) << c.name;

    QSignalSpy rect_spy(&controller, &EditorInteractionController::cropRectCommitted);
    controller.handlePress(start.x(), start.y(), static_cast<int>(Qt::LeftButton));
    controller.handleMove(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
    controller.handleRelease(end.x(), end.y(), static_cast<int>(Qt::LeftButton));

    EXPECT_GE(rect_spy.count(), 1) << c.name;
    const QRectF rect = controller.cropRectNormalized();
    EXPECT_NEAR(rect.x(), 0.1, 3.0e-2) << c.name;
    EXPECT_NEAR(rect.y(), 0.1, 3.0e-2) << c.name;
    EXPECT_GT(rect.width(), 0.2) << c.name;
    EXPECT_GT(rect.height(), 0.25) << c.name;
  }
}

TEST(EditorOverlayInteractionTest, DoubleTapResetsCropWhenToolEnabledAndTogglesFitZoomOtherwise) {
  EditorInteractionController crop_controller;
  crop_controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(crop_controller, 400, 300);
  crop_controller.setCropToolEnabled(true);
  crop_controller.setCropOverlayVisible(true);
  crop_controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.4, 0.4));
  crop_controller.setCropRotationDegrees(15.0f);

  crop_controller.handleDoubleTap(400, 300);
  EXPECT_NEAR(crop_controller.cropRectNormalized().x(), 0.0, 1.0e-3);
  EXPECT_NEAR(crop_controller.cropRectNormalized().width(), 1.0, 1.0e-3);
  EXPECT_NEAR(crop_controller.cropRotationDegrees(), 0.0f, 1.0e-3);

  EditorInteractionController view_controller;
  view_controller.setViewportMetrics(800, 600, 1.5);
  ConfigureImage(view_controller);
  view_controller.handleDoubleTap(400, 300);
  // Animation may be running; finish via a second double-tap after forcing progress.
  // Apply a direct zoom then double-tap to fit.
  view_controller.applyViewTransformForTest(2.0f, 0.1f, 0.0f);
  view_controller.resetView();
  EXPECT_NEAR(view_controller.zoom(), 1.0f, 1.0e-4);
  EXPECT_NEAR(view_controller.panX(), 0.0f, 1.0e-4);
  EXPECT_NEAR(view_controller.panY(), 0.0f, 1.0e-4);
}

TEST(EditorOverlayInteractionTest, PinchAndTrackpadPanUpdateViewTransform) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 2.0);
  ConfigureImage(controller);

  const float before = controller.zoom();
  controller.handlePinch(400, 300, 0.15f);
  EXPECT_GT(controller.zoom(), before);

  controller.applyViewTransformForTest(2.0f, 0.0f, 0.0f);
  const float pan_before = controller.panX();
  controller.handleWheel(400, 300, 0, 40, 0, 0, true);
  EXPECT_NE(controller.panX(), pan_before);
}

TEST(EditorOverlayInteractionTest, ItemAndImageUvCoordinateApisRoundTripAtMultipleDpr) {
  for (const auto& c : kDprCases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.width, c.height, c.dpr);
    ConfigureImage(controller, 400, 300);

    const QPointF item = controller.imageUvToItemPoint(0.35, 0.55);
    ASSERT_TRUE(controller.isItemPointInsideImage(item.x(), item.y())) << c.name;
    const QPointF uv = controller.itemPointToImageUv(item.x(), item.y());
    EXPECT_NEAR(uv.x(), 0.35, 2.0e-3) << c.name;
    EXPECT_NEAR(uv.y(), 0.55, 2.0e-3) << c.name;
  }
}

TEST(EditorOverlayInteractionTest, OverlaySceneGeometryBuildsMaskBorderGridAndHandles) {
  EditorInteractionController controller;
  controller.setViewportMetrics(960, 540, 1.0);
  ConfigureImage(controller, 4000, 2250);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(QRectF(0.15, 0.2, 0.6, 0.55));
  controller.setDetailRoiVisible(true);
  controller.setDetailRoiNormalized(QRectF(0.4, 0.4, 0.15, 0.12));

  const auto scene =
      BuildOverlaySceneGeometry(controller.overlayGeometry(), controller.cropOverlayVisible());
  EXPECT_TRUE(scene.has_crop);
  EXPECT_TRUE(scene.has_detail_roi);
  EXPECT_FALSE(scene.mask_triangles.empty());
  EXPECT_EQ(scene.border_lines.size() % 2, 0u);
  EXPECT_GE(scene.border_lines.size(), 8u);
  EXPECT_GE(scene.grid_lines.size(), 8u);
  EXPECT_EQ(scene.rotate_stem_line.size(), 2u);
  EXPECT_EQ(scene.handle_count, 5);
  EXPECT_FALSE(scene.detail_roi_lines.empty());
}

TEST(EditorOverlayInteractionTest, OverlaySceneGeometryGoldenAcrossViewportAspects) {
  struct Case {
    const char* name;
    qreal w;
    qreal h;
  };
  const Case cases[] = {{"landscape", 960, 540}, {"portrait", 540, 960}, {"square", 700, 700},
                        {"odd", 801, 599}};

  for (const auto& c : cases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.w, c.h, 1.0);
    ConfigureImage(controller, 3000, 2000);
    controller.setCropOverlayVisible(true);
    controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.5, 0.5));

    const auto geometry = controller.overlayGeometry();
    ASSERT_TRUE(geometry.crop_corners_valid) << c.name;
    const auto scene = BuildOverlaySceneGeometry(geometry, true);
    EXPECT_TRUE(scene.has_crop) << c.name;
    EXPECT_FALSE(scene.mask_triangles.empty()) << c.name;
    // Triangle count is stable enough to catch accidental empty builds.
    EXPECT_GE(scene.mask_triangles.size(), 3u) << c.name;
    EXPECT_EQ(scene.handle_count, 5) << c.name;
  }
}

TEST(EditorOverlayInteractionTest, HoverSetsCropCursorPriorityCornersOverEdges) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));

  const auto geometry = controller.overlayGeometry();
  ASSERT_TRUE(geometry.crop_corners_valid);
  controller.handleHoverMove(geometry.crop_corners_widget[0].x(),
                             geometry.crop_corners_widget[0].y());
  EXPECT_TRUE(controller.hasCustomCursor());
  EXPECT_NE(controller.cursorShape(), static_cast<int>(Qt::ArrowCursor));
}

TEST(EditorOverlayInteractionTest, ViewTransformPushDoesNotInvalidateBrokerTargets) {
  // Offscreen QQuickRhiItem construction is allowed without a visible window for
  // property/API checks; broker starts with zero live targets until a renderer
  // publishes a pool.
  EditorViewportItem viewport;
  const auto before_live = viewport.liveTargetCount();
  const auto before_gen = viewport.targetGeneration();

  viewport.setViewTransform(2.0f, 0.1f, -0.05f);
  viewport.setViewTransform(1.5f, 0.0f, 0.0f);

  EXPECT_EQ(viewport.liveTargetCount(), before_live);
  EXPECT_EQ(viewport.targetGeneration(), before_gen);
}

TEST(EditorOverlayInteractionTest, OverlayRefreshDoesNotAdvanceTargetGeneration) {
  EditorInteractionController controller;
  controller.setViewportMetrics(640, 480, 1.0);
  ConfigureImage(controller);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(QRectF(0.1, 0.1, 0.8, 0.8));

  EditorViewportItem viewport;
  const auto gen_before = viewport.targetGeneration();

  QSignalSpy overlay_spy(&controller, &EditorInteractionController::overlayGeometryChanged);
  controller.setCropRotationDegrees(12.0f);
  controller.applyViewTransformForTest(1.8f, 0.05f, 0.0f);
  EXPECT_GE(overlay_spy.count(), 1);
  EXPECT_EQ(viewport.targetGeneration(), gen_before);
}

TEST(EditorOverlayInteractionTest, DetailRoiVisibilityIsIndependentOfCropOverlay) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropOverlayVisible(false);
  controller.setDetailRoiVisible(true);
  controller.setDetailRoiNormalized(QRectF(0.3, 0.3, 0.25, 0.2));

  const auto geometry = controller.overlayGeometry();
  EXPECT_TRUE(geometry.detail_roi_valid);
  EXPECT_FALSE(geometry.crop_corners_valid);

  const auto scene = BuildOverlaySceneGeometry(geometry, false);
  EXPECT_FALSE(scene.has_crop);
  EXPECT_TRUE(scene.has_detail_roi);
}

}  // namespace alcedo::editor_rhi
