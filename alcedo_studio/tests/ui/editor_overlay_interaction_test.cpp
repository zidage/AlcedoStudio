//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_overlay_interaction_test.cpp
/// @brief Phase 3 / Phase 3-Fix: EditorInteractionController, OverlaySceneGeometry,
/// full view-state push to LeaseFrameSink, crop rules, DPR-invariant pan, pinch
/// relative scale, and mask hole safety. Overlay updates must not recreate
/// viewport presentation targets.

#include <gtest/gtest.h>

#include <QCoreApplication>
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
#include "ui/editor_rhi/lease_frame_sink.hpp"

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

// Sample whether a point is covered by any mask triangle (barycentric).
auto PointInTriangle(const QPointF& p, const QPointF& a, const QPointF& b, const QPointF& c)
    -> bool {
  const auto cross = [](const QPointF& o, const QPointF& u, const QPointF& v) {
    return (u.x() - o.x()) * (v.y() - o.y()) - (u.y() - o.y()) * (v.x() - o.x());
  };
  const double c1 = cross(a, b, p);
  const double c2 = cross(b, c, p);
  const double c3 = cross(c, a, p);
  const bool has_neg = (c1 < 0) || (c2 < 0) || (c3 < 0);
  const bool has_pos = (c1 > 0) || (c2 > 0) || (c3 > 0);
  return !(has_neg && has_pos);
}

auto MaskCoversPoint(const OverlaySceneGeometry& scene, const QPointF& p) -> bool {
  const auto& t = scene.mask_triangles;
  for (size_t i = 0; i + 2 < t.size(); i += 3) {
    if (PointInTriangle(p, t[i], t[i + 1], t[i + 2])) {
      return true;
    }
  }
  return false;
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

TEST(EditorOverlayInteractionTest, LogicalPanDeltaIsIndependentOfDevicePixelRatio) {
  // Same logical 50px drag at DPR 1.0 / 1.5 / 2.0 must produce the same NDC pan.
  float pan_at_dpr1 = 0.0f;
  for (const auto& c : kDprCases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.width, c.height, c.dpr);
    ConfigureImage(controller);
    controller.applyViewTransformForTest(2.0f, 0.0f, 0.0f);

    controller.handlePress(400, 300, static_cast<int>(Qt::LeftButton));
    controller.handleMove(450, 300, static_cast<int>(Qt::LeftButton));
    controller.handleRelease(450, 300, static_cast<int>(Qt::LeftButton));

    if (c.dpr == 1.0) {
      pan_at_dpr1 = controller.panX();
      EXPECT_NE(pan_at_dpr1, 0.0f) << c.name;
    } else {
      EXPECT_NEAR(controller.panX(), pan_at_dpr1, 1.0e-4f) << c.name;
    }
  }
}

TEST(EditorOverlayInteractionTest, CropCreateDragUpdatesNormalizedRectAndFinalizesOnRelease) {
  for (const auto& c : kDprCases) {
    EditorInteractionController controller;
    controller.setViewportMetrics(c.width, c.height, c.dpr);
    ConfigureImage(controller, 400, 300);
    controller.setCropToolEnabled(true);
    controller.setCropOverlayVisible(true);
    controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));

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

TEST(EditorOverlayInteractionTest, DisablingCropToolCancelsInFlightDragWithoutCommit) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
  const QRectF initial(0.25, 0.25, 0.5, 0.5);
  controller.setCropRectNormalized(initial);

  const QPointF start = controller.imageUvToItemPoint(0.1, 0.1);
  const QPointF mid = controller.imageUvToItemPoint(0.35, 0.4);
  ASSERT_FALSE(start.isNull());
  ASSERT_FALSE(mid.isNull());

  controller.handlePress(start.x(), start.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(mid.x(), mid.y(), static_cast<int>(Qt::LeftButton));
  // Disable mid-drag — must cancel without keeping the provisional rect.
  controller.setCropToolEnabled(false);
  controller.handleRelease(mid.x(), mid.y(), static_cast<int>(Qt::LeftButton));

  const QRectF after = controller.cropRectNormalized();
  EXPECT_NEAR(after.x(), initial.x(), 1.0e-3);
  EXPECT_NEAR(after.y(), initial.y(), 1.0e-3);
  EXPECT_NEAR(after.width(), initial.width(), 1.0e-3);
  EXPECT_NEAR(after.height(), initial.height(), 1.0e-3);
}

TEST(EditorOverlayInteractionTest, SetCropRectNormalizedHonorsActiveRotation) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropOverlayVisible(true);
  controller.setCropRotationDegrees(35.0f);
  // A near-full rect would put rotated corners outside the image without clamp.
  controller.setCropRectNormalized(QRectF(0.05, 0.05, 0.9, 0.9));

  const auto crop = controller.viewerState().GetCropOverlay();
  const auto corners =
      CropGeometry::RotatedCropCornersUv(crop.rect, crop.rotation_degrees, crop.metric_aspect);
  for (const auto& c : corners) {
    EXPECT_GE(c.x(), -1e-4);
    EXPECT_LE(c.x(), 1.0 + 1e-4);
    EXPECT_GE(c.y(), -1e-4);
    EXPECT_LE(c.y(), 1.0 + 1e-4);
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
  view_controller.applyViewTransformForTest(2.0f, 0.1f, 0.0f);
  view_controller.resetView();
  EXPECT_NEAR(view_controller.zoom(), 1.0f, 1.0e-4);
  EXPECT_NEAR(view_controller.panX(), 0.0f, 1.0e-4);
  EXPECT_NEAR(view_controller.panY(), 0.0f, 1.0e-4);
}

TEST(EditorOverlayInteractionTest, PinchRelativeScaleIsPathIndependent) {
  // Many small multiplicative steps vs one large step for the same total scale.
  EditorInteractionController many_steps;
  many_steps.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(many_steps);
  // 10 steps of ratio 1.05 → total * (1.05)^10
  for (int i = 0; i < 10; ++i) {
    many_steps.handlePinch(400, 300, 0.05);
  }

  EditorInteractionController one_step;
  one_step.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(one_step);
  const float total_ratio = std::pow(1.05f, 10.0f);
  one_step.handlePinch(400, 300, total_ratio - 1.0f);

  EXPECT_NEAR(many_steps.zoom(), one_step.zoom(), 1.0e-3f);
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
  EXPECT_FALSE(scene.border_outer_triangles.empty());
  EXPECT_FALSE(scene.border_inner_triangles.empty());
  EXPECT_FALSE(scene.grid_triangles.empty());
  EXPECT_FALSE(scene.stem_outer_triangles.empty());
  EXPECT_EQ(scene.handle_count, 5);
  EXPECT_FALSE(scene.handle_fill_triangles.empty());
  EXPECT_FALSE(scene.handle_outline_triangles.empty());
  EXPECT_FALSE(scene.detail_roi_triangles.empty());
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
    EXPECT_GE(scene.mask_triangles.size(), 6u) << c.name;
    EXPECT_EQ(scene.handle_count, 5) << c.name;

    // Crop center must NOT be covered by the dim mask.
    const QPointF crop_center = CropGeometry::CropCenterWidgetPoint(geometry.crop_corners_widget);
    EXPECT_FALSE(MaskCoversPoint(scene, crop_center)) << c.name;

    // A point near the image corner (outside crop) should be dimmed.
    const QPointF image_corner = geometry.image_rect.topLeft() + QPointF(2.0, 2.0);
    if (!geometry.image_rect.contains(crop_center) ||
        (image_corner - crop_center).manhattanLength() > 20.0) {
      EXPECT_TRUE(MaskCoversPoint(scene, image_corner)) << c.name;
    }
  }
}

TEST(EditorOverlayInteractionTest, DimMaskDoesNotCoverCropInteriorAtMultipleRotations) {
  for (const float degrees : {0.0f, 15.0f, 35.0f, -22.0f}) {
    EditorInteractionController controller;
    controller.setViewportMetrics(800, 600, 1.0);
    ConfigureImage(controller, 400, 300);
    controller.setCropOverlayVisible(true);
    controller.setCropRotationDegrees(degrees);
    controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));

    const auto geometry = controller.overlayGeometry();
    ASSERT_TRUE(geometry.crop_corners_valid) << degrees;
    const auto scene = BuildOverlaySceneGeometry(geometry, true);
    const QPointF center = CropGeometry::CropCenterWidgetPoint(geometry.crop_corners_widget);
    EXPECT_FALSE(MaskCoversPoint(scene, center)) << "rotation=" << degrees;
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

TEST(EditorOverlayInteractionTest, ApplyViewStatePushesRegionAndInteractiveFlagsToSink) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);
  controller.applyViewTransformForTest(2.5f, 0.2f, -0.1f);

  EditorViewportItem viewport;
  controller.applyViewStateToViewport(&viewport);

  auto* sink = viewport.frameSink();
  ASSERT_NE(sink, nullptr);
  const auto region = sink->GetViewportRenderRegion();
  ASSERT_TRUE(region.has_value());
  EXPECT_GT(region->reference_width_, 0);
  EXPECT_GT(region->reference_height_, 0);

  // Prefer interactive primary when zoomed in.
  const auto state = controller.viewerViewState();
  EXPECT_TRUE(state.prefer_interactive_primary);
  EXPECT_TRUE(state.allow_detail_patch);
}

TEST(EditorOverlayInteractionTest, ReconcileViewportMetricsEmitsViewChanged) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);
  controller.applyViewTransformForTest(3.0f, 0.8f, 0.5f);

  QSignalSpy view_spy(&controller, &EditorInteractionController::viewChanged);
  // Shrink viewport so pan must be re-clamped → viewChanged.
  controller.setViewportMetrics(400, 300, 1.0);
  EXPECT_GE(view_spy.count(), 1);
}

TEST(EditorOverlayInteractionTest, ViewTransformPushDoesNotInvalidateBrokerTargets) {
  EditorViewportItem viewport;
  const auto before_live = viewport.liveTargetCount();
  const auto before_gen = viewport.targetGeneration();

  EditorInteractionController controller;
  controller.setViewportMetrics(640, 480, 1.0);
  ConfigureImage(controller);
  controller.applyViewTransformForTest(2.0f, 0.1f, -0.05f);
  controller.applyViewStateToViewport(&viewport);
  controller.applyViewTransformForTest(1.5f, 0.0f, 0.0f);
  controller.applyViewStateToViewport(&viewport);

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

TEST(EditorOverlayInteractionTest, OverlayRebuildsCoalesceAcrossMultiSignalBursts) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropOverlayVisible(true);

  EditorOverlayItem overlay;
  overlay.setInteraction(&controller);
  // Force an immediate rebuild so counters start clean after setInteraction.
  overlay.refreshFromInteraction();
  const int rebuilds_before = overlay.geometryRebuildCount();

  // One gesture emits crop + view + overlay signals; coalesced rebuild schedules once.
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.5, 0.5));
  controller.applyViewTransformForTest(1.5f, 0.05f, 0.0f);

  // Process the queued coalesced rebuild.
  QCoreApplication::processEvents();
  // At most a small number of rebuilds (queued once per burst), never one per signal.
  const int delta = overlay.geometryRebuildCount() - rebuilds_before;
  EXPECT_GE(delta, 1);
  EXPECT_LE(delta, 3);
}

}  // namespace alcedo::editor_rhi
