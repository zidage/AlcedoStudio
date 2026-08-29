//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_overlay_interaction_test.cpp
/// @brief Phase 3 / Phase 3-Fix: EditorInteractionController, OverlaySceneGeometry,
/// full view-state push to DirectFrameSink, crop rules, DPR-invariant pan, pinch
/// relative scale, and mask hole safety. Overlay updates must not recreate
/// viewport presentation targets.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSignalSpy>
#include <QTest>
#include <QVector2D>
#include <Qt>
#include <cmath>
#include <vector>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"

namespace alcedo::editor_rhi {
namespace {

struct ViewportCase {
  const char* name;
  qreal       width;
  qreal       height;
  qreal       dpr;
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
  const double c1      = cross(a, b, p);
  const double c2      = cross(b, c, p);
  const double c3      = cross(c, a, p);
  const bool   has_neg = (c1 < 0) || (c2 < 0) || (c3 < 0);
  const bool   has_pos = (c1 > 0) || (c2 > 0) || (c3 > 0);
  return !(has_neg && has_pos);
}

auto MaskCoverageCount(const OverlaySceneGeometry& scene, const QPointF& p) -> int {
  const auto& t     = scene.mask_triangles;
  int         count = 0;
  for (size_t i = 0; i + 2 < t.size(); i += 3) {
    if (PointInTriangle(p, t[i], t[i + 1], t[i + 2])) {
      ++count;
    }
  }
  return count;
}

auto MaskCoversPoint(const OverlaySceneGeometry& scene, const QPointF& p) -> bool {
  return MaskCoverageCount(scene, p) > 0;
}

// Build a single stroke segment with round caps via grip geometry helpers.
// Horizontal grip from (x0,y) to (x1,y) is exercised through BuildOverlaySceneGeometry.
auto MaxExtentBeyondEndpoint(const std::vector<QPointF>& tris, const QPointF& endpoint,
                             const QPointF& outward_unit) -> double {
  double best = -1.0e9;
  for (const auto& p : tris) {
    const double proj =
        (p.x() - endpoint.x()) * outward_unit.x() + (p.y() - endpoint.y()) * outward_unit.y();
    best = std::max(best, proj);
  }
  return best;
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

TEST(EditorOverlayInteractionTest, TrueZoomIsOneToOneAtActualPixels) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);  // aspect-matched → fitFraction = 800/4000 = 0.2

  // At fit (zoom field 1.0), true zoom equals the fit fraction (20%).
  EXPECT_NEAR(controller.trueZoom(), 0.2f, 1.0e-5f);
  controller.zoomToActualPixels();
  // 1:1 → zoom field 1/0.2 = 5.0, true zoom == 1.0 (100%).
  EXPECT_NEAR(controller.zoom(), 5.0f, 1.0e-4f);
  EXPECT_NEAR(controller.trueZoom(), 1.0f, 1.0e-4f);
}

TEST(EditorOverlayInteractionTest, ActualPixelsClampsToFitWhenFitExceedsOneToOne) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);  // fitFraction = 800/400 = 2.0 → fit is 200%

  // 1:1 lies below the fit floor for this small image, so the shortcut clamps
  // to fit instead of zooming below it.
  EXPECT_NEAR(controller.trueZoom(), 2.0f, 1.0e-5f);
  controller.zoomToActualPixels();
  EXPECT_NEAR(controller.zoom(), 1.0f, 1.0e-5f);
  EXPECT_NEAR(controller.trueZoom(), 2.0f, 1.0e-5f);
}

TEST(EditorOverlayInteractionTest, MaxZoomClampsToTrueZoomCeilingNotLegacyField) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000,
                 3000);  // fitFraction 0.2 → field ceiling 80 → true ceiling 1600%

  // Saturate the cap with many zoom-in steps. The cap is expressed in true
  // zoom (1600%), not the legacy 8x fit field, so the field reaches ~80 and
  // true zoom reaches 16.0 — well past the old 8.0 field / 1.6 true ceiling.
  for (int i = 0; i < 200; ++i) {
    controller.handleWheel(400, 300, 120, 0, 0, static_cast<int>(Qt::ControlModifier), false);
  }
  EXPECT_NEAR(controller.trueZoom(), 16.0f, 0.05f);
  EXPECT_LE(controller.trueZoom(), 16.0f + 0.05f);
}

TEST(EditorOverlayInteractionTest, MaxZoomStillRoutesDetailRefreshAfterNoOpWheels) {
  // Regression: zooming to the ceiling then scrolling past it (no-op wheels)
  // must NOT cancel the DetailRefresh armed by the last real zoom step. The
  // Issue 1 no-op fix made ApplyViewTransform skip request_repaint, but the
  // wheel handler still stopped the settle timer up-front — so the pending
  // high-quality ROI never rendered until a drag. Wheel/pinch now preserve the
  // settle timer on a no-op; only a real view change supersedes it.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 800, 600);  // fit at 1.0 → field ceiling 16 (1600%)

  QSignalSpy change_spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(change_spy.isValid());

  // Climb to the ceiling. Each step is a real change that arms the settle timer
  // (no immediate viewChangeReported — the timer hasn't fired yet).
  for (int i = 0; i < 40; ++i) {
    controller.handleWheel(400, 300, 120, 0, 0, static_cast<int>(Qt::ControlModifier), false);
  }
  ASSERT_NEAR(controller.zoom(), 16.0f, 0.2f);  // saturated at the ceiling
  EXPECT_EQ(change_spy.count(), 0);             // settle armed, not yet fired

  // No-op wheels at the ceiling must NOT cancel the armed settle timer.
  for (int i = 0; i < 5; ++i) {
    controller.handleWheel(400, 300, 120, 0, 0, static_cast<int>(Qt::ControlModifier), false);
  }
  EXPECT_NEAR(controller.zoom(), 16.0f, 1.0e-3f);  // unchanged

  // Let the 120ms settle timer fire. The DetailRefresh must be routed — the
  // no-ops did not drop it.
  QTest::qWait(200);
  QCoreApplication::processEvents();

  ASSERT_GE(change_spy.count(), 1);
  EXPECT_EQ(change_spy.takeLast().at(0).toInt(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::DetailRefresh));
}

TEST(EditorOverlayInteractionTest, TrueZoomUsesCroppedOutputAfterCropCommit) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);  // source 4000x3000, fitFraction 0.2

  // Editing a crop (overlay visible): CROP_ROTATE is disabled, so the displayed
  // image is still the full source. Dragging the rect must NOT change true zoom.
  controller.setCropOverlayVisible(true);
  EXPECT_NEAR(controller.trueZoom(), 0.2f, 1.0e-5f);
  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));  // central 50% crop
  EXPECT_NEAR(controller.trueZoom(), 0.2f, 1.0e-5f);               // still source-based

  // Close the panel → crop commits (overlay hidden). Displayed image is now the
  // cropped output (round(4000*0.5) x round(3000*0.5) = 2000x1500, same aspect),
  // so fitFraction becomes 800/2000 = 0.4 and true zoom at fit is 40%.
  controller.setCropOverlayVisible(false);
  EXPECT_NEAR(controller.trueZoom(), 0.4f, 1.0e-5f);

  // 1:1 is now 1 cropped-output pixel per screen pixel → field 1/0.4 = 2.5.
  controller.zoomToActualPixels();
  EXPECT_NEAR(controller.zoom(), 2.5f, 1.0e-4f);
  EXPECT_NEAR(controller.trueZoom(), 1.0f, 1.0e-4f);

  // Reopening the panel switches back to the full source (1:1 field 5.0).
  controller.setCropOverlayVisible(true);
  EXPECT_NEAR(controller.trueZoom(), 0.5f, 1.0e-4f);  // 0.2 * 2.5
  controller.zoomToActualPixels();
  EXPECT_NEAR(controller.zoom(), 5.0f, 1.0e-4f);
  EXPECT_NEAR(controller.trueZoom(), 1.0f, 1.0e-4f);
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
    const QPointF end   = controller.imageUvToItemPoint(0.4, 0.45);
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
  const QPointF mid   = controller.imageUvToItemPoint(0.35, 0.4);
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

TEST(EditorOverlayInteractionTest, PinchAbsoluteToSurvivesScaleResetWithoutFitSnap) {
  // Qt PinchHandler.scale resets to 1.0 between pinch sequences. Incremental
  // scale/lastScale ratios mis-handle that reset and can multiply zoom by
  // ~1/previousEndScale, snapping to FIT. Absolute handlePinchTo (as used by
  // EditorWorkspace.qml) keeps zoom continuous across pinch restarts.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller);
  controller.applyViewTransformForTest(2.0f, 0.0f, 0.0f);

  // First pinch: baseZoom=2, scale 1.0 -> 1.3
  controller.handlePinchTo(400, 300, 2.0f * 1.0f);
  EXPECT_NEAR(controller.zoom(), 2.0f, 1.0e-3f);
  controller.handlePinchTo(400, 300, 2.0f * 1.3f);
  EXPECT_NEAR(controller.zoom(), 2.6f, 1.0e-3f);

  // Second pinch restart: re-base at current zoom with scale=1.0 (no change),
  // then pinch in further. Must not go to FIT.
  const float second_base = controller.zoom();
  controller.handlePinchTo(400, 300, second_base * 1.0f);
  EXPECT_NEAR(controller.zoom(), second_base, 1.0e-3f);
  controller.handlePinchTo(400, 300, second_base * 1.2f);
  EXPECT_NEAR(controller.zoom(), second_base * 1.2f, 1.0e-3f);
  EXPECT_GT(controller.zoom(), 1.0f + 1.0e-3f);

  // Contrast: the broken incremental interpretation of a scale reset
  // (factor = 1.0 / 1.3) would collapse 2.6 → 2.0, and 1.0/2.6 collapses to FIT.
  EditorInteractionController broken;
  broken.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(broken);
  broken.applyViewTransformForTest(2.6f, 0.0f, 0.0f);
  broken.handlePinch(400, 300, (1.0f / 2.6f) - 1.0f);
  EXPECT_NEAR(broken.zoom(), 1.0f, 1.0e-3f);
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
    qreal       w;
    qreal       h;
  };
  const Case cases[] = {
      {"landscape", 960, 540}, {"portrait", 540, 960}, {"square", 700, 700}, {"odd", 801, 599}};

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
    const auto    scene  = BuildOverlaySceneGeometry(geometry, true);
    const QPointF center = CropGeometry::CropCenterWidgetPoint(geometry.crop_corners_widget);
    EXPECT_FALSE(MaskCoversPoint(scene, center)) << "rotation=" << degrees;
  }
}

TEST(EditorOverlayInteractionTest, DimMaskExteriorPixelsCoveredExactlyOnceAtMultipleRotations) {
  for (const float degrees : {0.0f, 15.0f, 35.0f, -22.0f}) {
    EditorInteractionController controller;
    controller.setViewportMetrics(800, 600, 1.0);
    ConfigureImage(controller, 400, 300);
    controller.setCropOverlayVisible(true);
    controller.setCropRotationDegrees(degrees);
    controller.setCropRectNormalized(QRectF(0.3, 0.3, 0.4, 0.4));

    const auto geometry = controller.overlayGeometry();
    ASSERT_TRUE(geometry.crop_corners_valid) << degrees;
    const auto scene = BuildOverlaySceneGeometry(geometry, true);
    ASSERT_FALSE(scene.mask_triangles.empty()) << degrees;

    // Four exterior samples near image corners — each must be covered once so
    // alpha blending yields a uniform dim (no double-dark corners).
    const QPointF samples[] = {
        geometry.image_rect.topLeft() + QPointF(4.0, 4.0),
        geometry.image_rect.topRight() + QPointF(-4.0, 4.0),
        geometry.image_rect.bottomLeft() + QPointF(4.0, -4.0),
        geometry.image_rect.bottomRight() + QPointF(-4.0, -4.0),
    };
    int covered = 0;
    for (const auto& sample : samples) {
      const int count = MaskCoverageCount(scene, sample);
      if (count > 0) {
        EXPECT_EQ(count, 1) << "rotation=" << degrees << " sample=(" << sample.x() << ","
                            << sample.y() << ") coverage=" << count;
        ++covered;
      }
    }
    EXPECT_GE(covered, 2) << "rotation=" << degrees;
  }
}

TEST(EditorOverlayInteractionTest, RoundCapsExtendOutwardPastStrokeEndpoints) {
  // Axis-aligned crop so edge grips are horizontal/vertical with known endpoints.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.6, 0.6));

  const auto geometry = controller.overlayGeometry();
  ASSERT_TRUE(geometry.crop_corners_valid);
  const auto scene = BuildOverlaySceneGeometry(geometry, true);
  ASSERT_FALSE(scene.grip_outer_triangles.empty());

  // Top edge grip is the middle 24% of the top crop edge (0.38..0.62).
  const QPointF a   = CropGeometry::LerpPoint(geometry.crop_corners_widget[0],
                                              geometry.crop_corners_widget[1], 0.38f);
  const QPointF b   = CropGeometry::LerpPoint(geometry.crop_corners_widget[0],
                                              geometry.crop_corners_widget[1], 0.62f);
  const QPointF ab  = b - a;
  const double  len = std::hypot(ab.x(), ab.y());
  ASSERT_GT(len, 1.0);
  const QPointF outward_a(-ab.x() / len, -ab.y() / len);
  const QPointF outward_b(ab.x() / len, ab.y() / len);

  // Outer grip stroke width is 5.0 → radius 2.5. Caps must reach past endpoints.
  const double  reach_a = MaxExtentBeyondEndpoint(scene.grip_outer_triangles, a, outward_a);
  const double  reach_b = MaxExtentBeyondEndpoint(scene.grip_outer_triangles, b, outward_b);
  EXPECT_GT(reach_a, 1.5) << "left/top cap must extend outward past endpoint a";
  EXPECT_GT(reach_b, 1.5) << "right/top cap must extend outward past endpoint b";

  // Interior of the stroke (midpoint) must also have geometry, but endpoints
  // should not only have inward-only coverage: max inward reach past a toward b
  // for a pure inward bug is large; outward reach was the failing signal.
  const double inward_a = MaxExtentBeyondEndpoint(scene.grip_outer_triangles, a, outward_b);
  EXPECT_GT(inward_a, 0.0);
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

  // Receive-side verification: flags and transform live on the sink, not only
  // on the controller's local copy.
  const auto sink_state = sink->ViewState();
  EXPECT_TRUE(sink_state.prefer_interactive_primary);
  EXPECT_TRUE(sink_state.allow_detail_patch);
  EXPECT_NEAR(sink_state.snapshot.view_transform.zoom, 2.5f, 1.0e-4f);
  EXPECT_NEAR(sink_state.snapshot.view_transform.pan.x(), controller.panX(), 1.0e-4f);
  EXPECT_NEAR(sink_state.snapshot.view_transform.pan.y(), controller.panY(), 1.0e-4f);
}

TEST(EditorOverlayInteractionTest, SingleViewStatePushPerInputSequenceDespiteDualSignals) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);

  EditorViewportItem viewport;
  int                push_count = 0;
  QObject::connect(&controller, &EditorInteractionController::viewStateChanged, &controller, [&]() {
    controller.applyViewStateToViewport(&viewport);
    ++push_count;
  });
  // Intentionally do NOT connect viewChanged — production QML only listens to
  // viewStateChanged so dual emits cannot double-push.

  const int before = viewport.viewStatePushCount();
  controller.applyViewTransformForTest(2.0f, 0.1f, -0.05f);
  EXPECT_EQ(push_count, 1);
  EXPECT_EQ(viewport.viewStatePushCount() - before, 1);

  // Continuous drag: each move is one push, never two for the same state.
  controller.handlePress(400, 300, static_cast<int>(Qt::LeftButton));
  const int mid = viewport.viewStatePushCount();
  controller.handleMove(420, 310, static_cast<int>(Qt::LeftButton));
  controller.handleMove(440, 320, static_cast<int>(Qt::LeftButton));
  controller.handleRelease(440, 320, static_cast<int>(Qt::LeftButton));
  EXPECT_GE(viewport.viewStatePushCount() - mid, 1);
  // Two moves + release path should not emit more than a handful of state pushes.
  EXPECT_LE(viewport.viewStatePushCount() - mid, 4);
}

TEST(EditorOverlayInteractionTest, ResetPresentationStateClearsCropRoiAndMode) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.5, 0.5));
  controller.setCropRotationDegrees(18.0f);
  controller.setDetailRoiVisible(true);
  controller.setDetailRoiNormalized(QRectF(0.4, 0.4, 0.1, 0.1));
  controller.setPresentationMode(static_cast<int>(alcedo::FramePresentationMode::RoiFrame));
  controller.applyViewTransformForTest(2.5f, 0.2f, 0.1f);

  controller.resetPresentationStateForNewImage();

  EXPECT_FALSE(controller.cropToolEnabled());
  EXPECT_FALSE(controller.cropOverlayVisible());
  EXPECT_NEAR(controller.cropRectNormalized().x(), 0.0, 1.0e-5);
  EXPECT_NEAR(controller.cropRectNormalized().width(), 1.0, 1.0e-5);
  EXPECT_NEAR(controller.cropRotationDegrees(), 0.0f, 1.0e-4f);
  EXPECT_FALSE(controller.detailRoiVisible());
  EXPECT_EQ(controller.presentationMode(),
            static_cast<int>(alcedo::FramePresentationMode::FullFrame));
  EXPECT_NEAR(controller.zoom(), alcedo::ViewTransformController::kMinInteractiveZoom, 1.0e-4f);
  EXPECT_EQ(controller.renderReferenceWidth(), 0);
  EXPECT_EQ(controller.renderReferenceHeight(), 0);
}

// Phase 5B: two source images with different dimensions can share the same
// pipeline output size; EnsureSize must still re-emit targetSizeRequested so
// render-reference geometry is not stuck on the previous image's source size.
TEST(EditorOverlayInteractionTest, EqualOutputSizeImageSwitchResyncsRenderReference) {
  EditorViewportItem viewport;
  viewport.setImageIdentity(10);
  viewport.setSessionEpoch(1);

  int target_size_signals = 0;
  int last_w              = 0;
  int last_h              = 0;
  QObject::connect(&viewport, &EditorViewportItem::targetSizeRequested, &viewport,
                   [&](int w, int h) {
                     ++target_size_signals;
                     last_w = w;
                     last_h = h;
                   });

  auto* sink = viewport.frameSink();
  ASSERT_NE(sink, nullptr);

  // Image A: source 4000x3000, pipeline output 800x600.
  sink->EnsureSize(800, 600);
  EXPECT_EQ(target_size_signals, 1);
  EXPECT_EQ(last_w, 800);
  EXPECT_EQ(last_h, 600);

  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  controller.setImageSize(4000, 3000);
  controller.forceRenderReferenceSize(800, 600);
  EXPECT_EQ(controller.renderReferenceWidth(), 800);
  EXPECT_EQ(controller.renderReferenceHeight(), 600);

  // Switch to image B: different source size, same requested output size.
  viewport.setImageIdentity(20);
  viewport.setSessionEpoch(2);
  controller.resetPresentationStateForNewImage();
  controller.setImageSize(6000, 4000);
  // Interim source-size fallback (production QML path).
  controller.setRenderReferenceSize(6000, 4000);
  EXPECT_EQ(controller.renderReferenceWidth(), 6000);

  // EnsureSize with equal output dimensions must still re-emit for the new
  // session generation so forceRenderReferenceSize can restore pipeline size.
  sink->EnsureSize(800, 600);
  EXPECT_EQ(target_size_signals, 2);
  EXPECT_EQ(last_w, 800);
  EXPECT_EQ(last_h, 600);

  controller.forceRenderReferenceSize(last_w, last_h);
  EXPECT_EQ(controller.renderReferenceWidth(), 800);
  EXPECT_EQ(controller.renderReferenceHeight(), 600);

  // Same generation + same size is a true no-op.
  sink->EnsureSize(800, 600);
  EXPECT_EQ(target_size_signals, 2);
}

TEST(EditorOverlayInteractionTest, ForceRenderReferenceSizeReappliesEqualDimensions) {
  EditorInteractionController controller;
  controller.setViewportMetrics(640, 480, 1.0);
  controller.setImageSize(1000, 800);
  controller.setRenderReferenceSize(512, 384);
  EXPECT_EQ(controller.renderReferenceWidth(), 512);

  QSignalSpy geometry_spy(&controller, &EditorInteractionController::imageGeometryChanged);
  // setRenderReferenceSize with equal size is a no-op.
  controller.setRenderReferenceSize(512, 384);
  EXPECT_EQ(geometry_spy.count(), 0);

  // forceRenderReferenceSize always notifies (equal-output-size image switch).
  controller.forceRenderReferenceSize(512, 384);
  EXPECT_GE(geometry_spy.count(), 1);
  EXPECT_EQ(controller.renderReferenceWidth(), 512);
  EXPECT_EQ(controller.renderReferenceHeight(), 384);
}

TEST(EditorOverlayInteractionTest, RenderReferenceChangeNeverFeedsBackIntoDetailRendering) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 4000, 3000);
  controller.zoomToActualPixels();
  ASSERT_GT(controller.zoom(), 1.0f + 1.0e-4f);

  QSignalSpy change_spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(change_spy.isValid());

  // Render-reference geometry is output from the presentation pipeline. It may
  // update coordinate mapping, but it must never become a new pipeline input;
  // otherwise DetailPatch completion creates a self-sustaining render loop.
  controller.setRenderReferenceSize(2000, 1500);
  EXPECT_EQ(change_spy.count(), 0);
  QTest::qWait(200);
  QCoreApplication::processEvents();
  EXPECT_EQ(change_spy.count(), 0);
}

// DetailPatch ROI EnsureSize must not rewrite render-reference geometry. If it
// does, zoom/pan math and SameRoi matching break and the high-res detail patch
// no longer covers the viewport after double-click zoom.
TEST(EditorOverlayInteractionTest, DetailPatchEnsureSizeDoesNotRewriteRenderReference) {
  EditorViewportItem viewport;
  viewport.setImageIdentity(42);
  viewport.setSessionEpoch(1);

  int target_size_signals = 0;
  int last_w              = 0;
  int last_h              = 0;
  QObject::connect(&viewport, &EditorViewportItem::targetSizeRequested, &viewport,
                   [&](int w, int h) {
                     ++target_size_signals;
                     last_w = w;
                     last_h = h;
                   });

  auto* sink = viewport.frameSink();
  ASSERT_NE(sink, nullptr);

  const bool metal_present =
      alcedo::editor_rhi::ActiveEditorBackend() == alcedo::editor_rhi::EditorBackend::Metal;

  // QualityBase full frame establishes the render reference (e.g. 4K preview).
  alcedo::FramePreviewMetadata quality_meta{};
  quality_meta.frame_role         = alcedo::FrameRole::QualityBase;
  quality_meta.preview_generation = 1;
  sink->BindFrameSubmission({quality_meta, alcedo::FramePresentationMode::ViewportTransformed});
  sink->EnsureSize(2048, 1536);
  // CUDA/OpenCL: EnsureSize of a shared write target publishes render-ref size.
  // Metal zero-copy: EnsureSize is presentation-viewport bookkeeping only;
  // render-ref is published later from SubmitMetalFrame with the real texture.
  if (metal_present) {
    EXPECT_EQ(target_size_signals, 0);
#ifdef HAVE_METAL
    const auto owner = std::make_shared<int>(1);
    sink->SubmitMetalFrame(
        alcedo::ViewerMetalFrame{2048,
                                 1536,
                                 reinterpret_cast<std::uintptr_t>(owner.get()),
                                 std::shared_ptr<const void>(owner, owner.get()),
                                 {},
                                 alcedo::FramePresentationMode::ViewportTransformed,
                                 quality_meta});
    EXPECT_EQ(target_size_signals, 1);
    EXPECT_EQ(last_w, 2048);
    EXPECT_EQ(last_h, 1536);
#endif
  } else {
    EXPECT_EQ(target_size_signals, 1);
    EXPECT_EQ(last_w, 2048);
    EXPECT_EQ(last_h, 1536);
  }

  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  controller.setImageSize(4000, 3000);
  controller.forceRenderReferenceSize(last_w, last_h);
  EXPECT_EQ(controller.renderReferenceWidth(), 2048);
  EXPECT_EQ(controller.renderReferenceHeight(), 1536);

  // Zoomed DetailPatch is smaller than the full-frame reference (viewport ROI).
  alcedo::FramePreviewMetadata detail_meta{};
  detail_meta.frame_role         = alcedo::FrameRole::DetailPatch;
  detail_meta.preview_generation = 1;
  detail_meta.source_roi_norm    = {.x = 0.25f, .y = 0.2f, .width = 0.5f, .height = 0.4f};
  sink->BindFrameSubmission({detail_meta, alcedo::FramePresentationMode::ViewportTransformed});
  sink->EnsureSize(1600, 900);

  // Must not emit targetSizeRequested — render reference stays full-frame.
  if (metal_present) {
    EXPECT_EQ(target_size_signals, 1);
  } else {
    EXPECT_EQ(target_size_signals, 1);
  }
  EXPECT_EQ(last_w, 2048);
  EXPECT_EQ(last_h, 1536);
  EXPECT_EQ(controller.renderReferenceWidth(), 2048);
  EXPECT_EQ(controller.renderReferenceHeight(), 1536);

  // CUDA/OpenCL track the detail write size for MapResourceForWrite.
  // Metal does not allocate shared write slots; Detail EnsureSize must not
  // overwrite the last full-frame bookkeeping size used for change detection.
  if (metal_present) {
    EXPECT_EQ(sink->GetWidth(), 2048);
    EXPECT_EQ(sink->GetHeight(), 1536);
  } else {
    EXPECT_EQ(sink->GetWidth(), 1600);
    EXPECT_EQ(sink->GetHeight(), 900);
  }
}

TEST(EditorOverlayInteractionTest, ReleasingStaleDetailSlotUnblocksReplacementDetailTarget) {
  DirectPresentQueue queue(EditorBackend::Cuda);
  queue.InvalidateSessionEpoch(1, 42);
  queue.SetConsumerAvailable(true);

  const auto occupy_layer = [&](FrameRole role, std::uintptr_t handle) {
    constexpr int width    = 1600;
    constexpr int height   = 900;
    const auto    prepared = queue.PrepareWrite(width, height, 1, 42);
    EXPECT_TRUE(prepared.ok);
    EXPECT_TRUE(prepared.need_create);

    DirectPresentQueue::SlotNative native{};
    native.backend           = EditorBackend::Cuda;
    native.handle_kind       = LeaseNativeHandleKind::D3D11Texture2D;
    native.writable_kind     = LeaseWritableResourceKind::CudaArray;
    native.native_handle     = handle;
    native.writable_resource = handle + 100;
    EXPECT_TRUE(queue.PublishCreatedSlot(prepared.slot_index, width, height, native, 1, 42));
    EXPECT_TRUE(queue.BeginWrite(prepared.slot_index).has_value());
    queue.EndWrite(prepared.slot_index);
    FramePreviewMetadata metadata{};
    metadata.frame_role = role;
    queue.NotifyReady(prepared.slot_index, FramePresentationMode::ViewportTransformed, metadata);
    const auto consumed = queue.ConsumeNewestReady(role, 1, 42);
    EXPECT_TRUE(consumed.has_value());
    return consumed ? consumed->slot.index : -1;
  };

  const int interactive_slot = occupy_layer(FrameRole::InteractivePrimary, 1);
  const int quality_slot     = occupy_layer(FrameRole::QualityBase, 2);
  const int detail_slot      = occupy_layer(FrameRole::DetailPatch, 3);
  ASSERT_GE(interactive_slot, 0);
  ASSERT_GE(quality_slot, 0);
  ASSERT_GE(detail_slot, 0);

  // Three persistent renderer layers occupy the fixed three-slot queue.
  EXPECT_FALSE(queue.PrepareWrite(1600, 900, 1, 42).ok);

  // synchronize() now performs this release as soon as the view ROI differs.
  queue.CompleteRendererRead(detail_slot);
  const auto replacement = queue.PrepareWrite(1600, 900, 1, 42);
  EXPECT_TRUE(replacement.ok);
  EXPECT_FALSE(replacement.need_create);
  EXPECT_EQ(replacement.slot_index, detail_slot);

  queue.CompleteRendererRead(interactive_slot);
  queue.CompleteRendererRead(quality_slot);
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

TEST(EditorOverlayInteractionTest, ViewTransformPushDoesNotInvalidateDirectPresentTargets) {
  EditorViewportItem viewport;
  // Install a synthetic target generation so the comparison is not 0 == 0 vacuously.
  // View-state pushes must not advance direct-present target generation.
  if (viewport.present_queue()) {
    DirectPresentQueue::SizeRequest req;
    req.width          = 64;
    req.height         = 48;
    req.session_epoch  = 1;
    req.image_identity = 1;
    viewport.present_queue()->NoteSizeRequest(req);
    viewport.present_queue()->InvalidateTargetGeneration();
  }
  const auto before_live = viewport.liveTargetCount();
  const auto before_gen  = viewport.targetGeneration();
  EXPECT_GT(before_gen, 0u);

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
  const auto         gen_before = viewport.targetGeneration();

  QSignalSpy         overlay_spy(&controller, &EditorInteractionController::overlayGeometryChanged);
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

  // One drag emits crop + view + overlay signals; coalesced rebuild schedules once.
  controller.setCropRectNormalized(QRectF(0.2, 0.2, 0.5, 0.5));
  controller.applyViewTransformForTest(1.5f, 0.05f, 0.0f);

  // Process the queued coalesced rebuild.
  QCoreApplication::processEvents();
  // One logical change burst must schedule exactly one rebuild (not one per signal).
  const int delta = overlay.geometryRebuildCount() - rebuilds_before;
  EXPECT_EQ(delta, 1);
}

TEST(EditorOverlayInteractionTest, DoubleTapZoomReportsDetailRefreshOnlyAfterAnimationSettles) {
  // Double-click zoom animates for ~170ms. Progress ticks must re-sample the
  // view (viewStateChanged) without routing DetailRefresh; only the settled
  // finished frame schedules a DetailPatch. Spamming DetailRefresh mid-animation
  // used to deadlock the production cancel path and starve all pipeline work.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 800, 600);

  QSignalSpy change_spy(&controller, &EditorInteractionController::viewChangeReported);
  QSignalSpy state_spy(&controller, &EditorInteractionController::viewStateChanged);
  ASSERT_TRUE(change_spy.isValid());
  ASSERT_TRUE(state_spy.isValid());

  controller.handleDoubleTap(400, 300);
  // Animation just started: view may have been pushed, but no DetailRefresh yet.
  EXPECT_EQ(change_spy.count(), 0);

  // Drive the animation to completion (valueChanged + finished).
  QCoreApplication::processEvents();
  // Allow the 170ms zoom animation and 120ms settled-interaction debounce.
  QTest::qWait(350);
  QCoreApplication::processEvents();

  ASSERT_GT(controller.zoom(), 1.0f + 1.0e-4f);
  ASSERT_GE(state_spy.count(), 1);
  ASSERT_EQ(change_spy.count(), 1);
  EXPECT_EQ(change_spy.takeLast().at(0).toInt(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::DetailRefresh));
}

TEST(EditorOverlayInteractionTest, ViewChangeReportedForZoomPanCropRotateAndResize) {
  // Phase 5D D2: input handlers only report the new view via viewChangeReported;
  // they never choose or submit pipeline tasks. The kind tells the session how
  // to route: ZoomPan/Resize reuse the current full frame; CropRotate needs a
  // fresh InteractivePrimary; DetailRefresh (a zoomed viewport ROI) needs a
  // DetailPatch. viewChangeReported is emitted AFTER viewStateChanged so the QML
  // view push (and sink region) lands before the session routes the intent.
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 800, 600);  // aspect-matched → fit at zoom 1.0

  QSignalSpy spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(spy.isValid());
  auto last_kind = [&] { return spy.takeLast().at(0).toInt(); };

  // Ctrl+wheel zoom in until the viewport shows an ROI (zoom > 1) → DetailRefresh.
  for (int i = 0; i < 4 && controller.zoom() <= 1.0f + 1.0e-4f; ++i) {
    controller.handleWheel(400, 300, 120, 0, 0, static_cast<int>(Qt::ControlModifier), false);
  }
  ASSERT_GT(controller.zoom(), 1.0f + 1.0e-4f);
  QTest::qWait(150);
  QCoreApplication::processEvents();
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(last_kind(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::DetailRefresh));

  // Pan while zoomed → the detail patch must follow the new ROI → DetailRefresh.
  spy.clear();
  controller.handlePress(400, 300, static_cast<int>(Qt::LeftButton));
  controller.handleMove(450, 320, static_cast<int>(Qt::LeftButton));
  controller.handleMove(470, 330, static_cast<int>(Qt::LeftButton));
  controller.handleMove(490, 340, static_cast<int>(Qt::LeftButton));
  controller.handleRelease(490, 340, static_cast<int>(Qt::LeftButton));
  EXPECT_TRUE(spy.empty());
  QTest::qWait(150);
  QCoreApplication::processEvents();
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(last_kind(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::DetailRefresh));

  // Reset to fit (zoom ≤ 1) → the full frame is reused → ZoomPan.
  spy.clear();
  controller.resetView();
  ASSERT_LE(controller.zoom(), 1.0f + 1.0e-4f);
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(last_kind(), static_cast<int>(EditorInteractionController::ViewChangeKind::ZoomPan));

  // Crop rect change with overlay closed → baked content changes → CropRotate.
  spy.clear();
  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(last_kind(), static_cast<int>(EditorInteractionController::ViewChangeKind::CropRotate));

  // Crop rotation change with overlay closed → CropRotate.
  spy.clear();
  controller.setCropRotationDegrees(15.0f);
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(last_kind(), static_cast<int>(EditorInteractionController::ViewChangeKind::CropRotate));

  // Viewport metrics change (resize) → Resize (rebuild render-pass QRhi objects,
  // reuse the current full frame).
  spy.clear();
  controller.setViewportMetrics(1024, 768, 1.0);
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(last_kind(), static_cast<int>(EditorInteractionController::ViewChangeKind::Resize));
}

TEST(EditorOverlayInteractionTest, GeometryOverlayCropDraftDoesNotRouteCropRotateViewChanges) {
  // While the geometry overlay is open, crop-frame edits are pure UI over the
  // source-frame preview. Pipeline bake is owned by panel confirm (leave/Enter).
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);

  QSignalSpy spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(spy.isValid());
  QSignalSpy rect_spy(&controller, &EditorInteractionController::cropRectCommitted);
  ASSERT_TRUE(rect_spy.isValid());
  QSignalSpy rot_spy(&controller, &EditorInteractionController::cropRotationCommitted);
  ASSERT_TRUE(rot_spy.isValid());

  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));
  EXPECT_NEAR(controller.cropRectNormalized().x(), 0.25, 1e-4);
  EXPECT_GE(rect_spy.count(), 1);
  EXPECT_TRUE(spy.empty()) << "draft overlay rect must not emit viewChangeReported";

  spy.clear();
  controller.setCropRotationDegrees(12.0f);
  EXPECT_NEAR(controller.cropRotationDegrees(), 12.0f, 1e-4f);
  EXPECT_GE(rot_spy.count(), 1);
  EXPECT_TRUE(spy.empty()) << "draft overlay rotation must not emit viewChangeReported";

  // Drag a new crop rect over the source frame; intermediate moves + release
  // must stay local (no CropRotate routing).
  spy.clear();
  rect_spy.clear();
  const QPointF start = controller.imageUvToItemPoint(0.1, 0.1);
  const QPointF end   = controller.imageUvToItemPoint(0.4, 0.45);
  ASSERT_FALSE(start.isNull());
  ASSERT_FALSE(end.isNull());
  controller.handlePress(start.x(), start.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  controller.handleRelease(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_GE(rect_spy.count(), 1);
  EXPECT_TRUE(spy.empty()) << "crop-frame drag must not route pipeline view changes";

  // Closing the overlay restores content routing for subsequent crop commits.
  controller.setCropOverlayVisible(false);
  controller.setCropToolEnabled(false);
  spy.clear();
  controller.setCropRectNormalized(QRectF(0.1, 0.1, 0.8, 0.8));
  ASSERT_FALSE(spy.empty());
  EXPECT_EQ(spy.takeLast().at(0).toInt(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::CropRotate));
}

}  // namespace alcedo::editor_rhi
