//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>

#include "ui/edit_viewer/crop_geometry.hpp"
#include "ui/edit_viewer/crop_interaction_controller.hpp"
#include "ui/edit_viewer/edit_viewer_overlay_geometry.hpp"
#include "ui/edit_viewer/edit_viewer_surface.hpp"
#include "ui/edit_viewer/view_transform_controller.hpp"
#include "ui/edit_viewer/viewer_state.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {
namespace {

const ViewportWidgetInfo kWidgetInfo{800, 600, 1.0f};
const ViewportImageInfo  kImageInfo{400, 300};

auto                     WidgetPointForUv(const QPointF& uv) -> QPointF {
  const auto point = ViewportMapper::ImageUvToWidgetPoint(uv, kWidgetInfo, kImageInfo, 1.0f,
                                                                              QVector2D(0.0f, 0.0f));
  EXPECT_TRUE(point.has_value());
  return point.value_or(QPointF());
}

}  // namespace

TEST(EditViewerLogicTests, ViewportMapperRoundTripsUvThroughWidgetSpace) {
  const QPointF uv(0.23, 0.67);
  const auto widget_point = ViewportMapper::ImageUvToWidgetPoint(uv, kWidgetInfo, kImageInfo, 1.8f,
                                                                 QVector2D(0.1f, -0.2f));
  ASSERT_TRUE(widget_point.has_value());

  const auto round_trip = ViewportMapper::WidgetPointToImageUv(
      *widget_point, kWidgetInfo, kImageInfo, 1.8f, QVector2D(0.1f, -0.2f));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_NEAR(round_trip->x(), uv.x(), 1e-5);
  EXPECT_NEAR(round_trip->y(), uv.y(), 1e-5);
}

TEST(EditViewerLogicTests, ViewportRenderRegionCarriesReferenceSizeForDetailPreviewRequests) {
  const auto region = ViewportMapper::ComputeViewportRenderRegion(
      kWidgetInfo, 2.0f, QVector2D(0.15f, -0.1f), 4096, 3072);
  ASSERT_TRUE(region.has_value());
  EXPECT_EQ(region->reference_width_, 4096);
  EXPECT_EQ(region->reference_height_, 3072);
  EXPECT_EQ(region->target_width_, 800);
  EXPECT_EQ(region->target_height_, 600);
  EXPECT_LT(region->scale_x_, 1.0f);
  EXPECT_LT(region->scale_y_, 1.0f);
}

TEST(EditViewerLogicTests, ViewportRenderRegionTargetsVisibleImagePixels) {
  const ViewportWidgetInfo widget_info{800, 600, 2.0f};

  const auto               fit_region = ViewportMapper::ComputeViewportRenderRegion(
      widget_info, 1.0f, QVector2D(0.0f, 0.0f), 4000, 2000);
  ASSERT_TRUE(fit_region.has_value());
  EXPECT_EQ(fit_region->target_width_, 1600);
  EXPECT_EQ(fit_region->target_height_, 800);

  const auto zoom_region = ViewportMapper::ComputeViewportRenderRegion(
      widget_info, 2.0f, QVector2D(0.0f, 0.0f), 4000, 2000);
  ASSERT_TRUE(zoom_region.has_value());
  EXPECT_EQ(zoom_region->target_width_, 1600);
  EXPECT_EQ(zoom_region->target_height_, 1200);
}

TEST(EditViewerLogicTests,
     DirectPresentQueuePreservesQualityBeforeDetailRoiWhenBothFinishBeforePaint) {
  DirectPresentFrameQueue             queue;
  std::array<FramePreviewMetadata, 3> slot_metadata{};

  FramePreviewMetadata                quality_metadata{};
  quality_metadata.frame_role         = FrameRole::QualityBase;
  quality_metadata.preview_generation = 42;

  FramePreviewMetadata detail_metadata{};
  detail_metadata.frame_role         = FrameRole::DetailPatch;
  detail_metadata.preview_generation = 42;
  detail_metadata.detail_serial      = 7;
  detail_metadata.source_roi_norm    = {.x = 0.25f, .y = 0.2f, .width = 0.5f, .height = 0.4f};

  slot_metadata[1]                   = quality_metadata;
  slot_metadata[2]                   = detail_metadata;
  ASSERT_TRUE(queue.MarkReadySlot(1));
  ASSERT_TRUE(queue.MarkReadySlot(2));

  const auto quality_slot = queue.PopNextSlot();
  ASSERT_TRUE(quality_slot.has_value());
  ASSERT_EQ(*quality_slot, 1);
  EXPECT_EQ(slot_metadata[*quality_slot].frame_role, FrameRole::QualityBase);

  const auto detail_slot = queue.PopNextSlot();
  ASSERT_TRUE(detail_slot.has_value());
  ASSERT_EQ(*detail_slot, 2);
  EXPECT_EQ(slot_metadata[*detail_slot].frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(slot_metadata[*detail_slot].preview_generation,
            slot_metadata[*quality_slot].preview_generation);
  EXPECT_EQ(slot_metadata[*detail_slot].detail_serial, 7);
  EXPECT_TRUE(queue.Empty());
}

TEST(EditViewerLogicTests, DirectPresentQueueExposesPendingReferenceFramesWithoutPopping) {
  DirectPresentFrameQueue queue;

  ASSERT_TRUE(queue.MarkReadySlot(1));
  ASSERT_TRUE(queue.MarkReadySlot(2));

  const auto pending = queue.PendingSlotsSnapshot();
  ASSERT_EQ(pending.size(), 2U);
  EXPECT_EQ(pending[0], 1);
  EXPECT_EQ(pending[1], 2);
  EXPECT_EQ(queue.PendingCount(), 2U);

  const auto first = queue.PopNextSlot();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);
}

TEST(EditViewerLogicTests, RoiFramesDoNotBecomeRenderReferenceFrames) {
  EXPECT_TRUE(
      IsRenderReferenceFrame(FramePresentationMode::ViewportTransformed, FrameRole::QualityBase));
  EXPECT_TRUE(
      IsRenderReferenceFrame(FramePresentationMode::FullFrame, FrameRole::InteractivePrimary));
  EXPECT_FALSE(
      IsRenderReferenceFrame(FramePresentationMode::RoiFrame, FrameRole::InteractivePrimary));
  EXPECT_FALSE(
      IsRenderReferenceFrame(FramePresentationMode::ViewportTransformed, FrameRole::DetailPatch));
}

TEST(EditViewerLogicTests, CropGeometryAspectLockedDiagonalPreservesRatio) {
  const QRectF rect = CropGeometry::MakeAspectLockedRectFromDiagonal(
      QPointF(0.2, 0.2), QPointF(0.7, 0.5), 4.0f / 3.0f, 16.0f / 9.0f);
  ASSERT_GT(rect.width(), 0.0);
  ASSERT_GT(rect.height(), 0.0);
  EXPECT_NEAR(
      (static_cast<float>(rect.width()) * (4.0f / 3.0f)) / static_cast<float>(rect.height()),
      16.0f / 9.0f, 1e-4f);
}

TEST(EditViewerLogicTests, CropGeometryRotationClampKeepsCornersInsideNormalizedImage) {
  const QRectF rect =
      CropGeometry::ClampCropRectForRotation(QRectF(0.0, 0.0, 1.0, 1.0), 37.0f, 4.0f / 3.0f);
  const auto corners = CropGeometry::RotatedCropCornersUv(rect, 37.0f, 4.0f / 3.0f);
  for (const auto& corner : corners) {
    EXPECT_GE(corner.x(), -1e-5);
    EXPECT_LE(corner.x(), 1.0 + 1e-5);
    EXPECT_GE(corner.y(), -1e-5);
    EXPECT_LE(corner.y(), 1.0 + 1e-5);
  }
}

TEST(EditViewerLogicTests, ViewTransformControllerCtrlWheelUpdatesZoomAndPan) {
  ViewerState             state;
  ViewTransformController controller;

  const auto              result =
      controller.HandleCtrlWheel(state, kWidgetInfo, kImageInfo, 120, QPointF(400.0, 300.0));
  EXPECT_TRUE(result.consumed);
  EXPECT_TRUE(result.request_repaint);
  ASSERT_TRUE(result.emitted_zoom.has_value());
  EXPECT_GT(*result.emitted_zoom, 1.0f);
  EXPECT_GT(state.GetViewZoom(), 1.0f);
}

TEST(EditViewerLogicTests, ViewTransformPanDeltaMatchesAcrossDevicePixelRatios) {
  ViewerState state_dpr1;
  ViewerState state_dpr2;
  ViewTransformController controller_a;
  ViewTransformController controller_b;

  // Zoom in so pan is free to move.
  state_dpr1.SetViewTransform(2.0f, QVector2D(0.0f, 0.0f));
  state_dpr2.SetViewTransform(2.0f, QVector2D(0.0f, 0.0f));

  const ViewportWidgetInfo widget_dpr1{800, 600, 1.0f};
  const ViewportWidgetInfo widget_dpr2{800, 600, 2.0f};

  (void)controller_a.HandlePanPress(false, QPoint(400, 300));
  (void)controller_a.HandlePanMove(state_dpr1, widget_dpr1, kImageInfo, QPoint(450, 300));

  (void)controller_b.HandlePanPress(false, QPoint(400, 300));
  (void)controller_b.HandlePanMove(state_dpr2, widget_dpr2, kImageInfo, QPoint(450, 300));

  EXPECT_NEAR(state_dpr1.GetViewTransform().pan.x(), state_dpr2.GetViewTransform().pan.x(),
              1.0e-5f);
  EXPECT_NEAR(state_dpr1.GetViewTransform().pan.y(), state_dpr2.GetViewTransform().pan.y(),
              1.0e-5f);
}

TEST(EditViewerLogicTests, ViewTransformControllerDoubleClickStartsAnimationAndReturnsToFit) {
  ViewerState             state;
  ViewTransformController controller;

  const auto              zoom_in =
      controller.HandleDoubleClick(state, kWidgetInfo, kImageInfo, QPointF(400.0, 300.0));
  EXPECT_TRUE(zoom_in.start_animation);
  const auto progress = controller.ApplyAnimationFinished(state, kWidgetInfo, kImageInfo);
  ASSERT_TRUE(progress.emitted_zoom.has_value());
  EXPECT_GT(*progress.emitted_zoom, 1.0f);

  const auto zoom_out =
      controller.HandleDoubleClick(state, kWidgetInfo, kImageInfo, QPointF(400.0, 300.0));
  EXPECT_TRUE(zoom_out.start_animation);
  const auto finished = controller.ApplyAnimationFinished(state, kWidgetInfo, kImageInfo);
  ASSERT_TRUE(finished.emitted_zoom.has_value());
  EXPECT_FLOAT_EQ(*finished.emitted_zoom, 1.0f);
}

TEST(EditViewerLogicTests, AnchoredPanMustUseReferenceImageGeometryForViewportRoiViews) {
  const ViewportWidgetInfo widget_info{800, 600, 1.0f};
  const ViewportImageInfo  reference_image{6000, 4000};
  const QVector2D          current_pan(0.6f, 0.1f);
  constexpr float          current_zoom = 1.5f;
  constexpr float          target_zoom  = 2.0f;
  const QPointF            anchor_widget_pos(100.0, 500.0);

  const auto               viewport_region = ViewportMapper::ComputeViewportRenderRegion(
      widget_info, current_zoom, current_pan, reference_image.image_width,
      reference_image.image_height);
  ASSERT_TRUE(viewport_region.has_value());

  const ViewportImageInfo roi_image{
      std::max(1, static_cast<int>(std::lround(static_cast<double>(reference_image.image_width) *
                                               viewport_region->scale_x_))),
      std::max(1, static_cast<int>(std::lround(static_cast<double>(reference_image.image_height) *
                                               viewport_region->scale_y_)))};

  const auto anchor_uv_before = ViewportMapper::WidgetPointToImageUv(
      anchor_widget_pos, widget_info, reference_image, current_zoom, current_pan);
  ASSERT_TRUE(anchor_uv_before.has_value());

  const auto unclamped_reference_pan =
      ViewportMapper::ComputeAnchoredPan(anchor_widget_pos, widget_info, reference_image,
                                         current_zoom, current_pan, target_zoom, current_pan);
  const auto reference_pan = ViewportMapper::ClampPanForZoom(
      widget_info, reference_image, target_zoom, unclamped_reference_pan, 1.0f, 8.0f);
  const auto anchored_uv_with_reference = ViewportMapper::WidgetPointToImageUv(
      anchor_widget_pos, widget_info, reference_image, target_zoom, reference_pan);
  ASSERT_TRUE(anchored_uv_with_reference.has_value());
  EXPECT_LT(std::abs(anchored_uv_with_reference->x() - anchor_uv_before->x()), 2.0e-2);
  EXPECT_NEAR(anchored_uv_with_reference->y(), anchor_uv_before->y(), 1.0e-5);

  const auto unclamped_roi_pan =
      ViewportMapper::ComputeAnchoredPan(anchor_widget_pos, widget_info, roi_image, current_zoom,
                                         current_pan, target_zoom, current_pan);
  const auto roi_pan = ViewportMapper::ClampPanForZoom(widget_info, roi_image, target_zoom,
                                                       unclamped_roi_pan, 1.0f, 8.0f);
  const auto anchored_uv_with_roi = ViewportMapper::WidgetPointToImageUv(
      anchor_widget_pos, widget_info, reference_image, target_zoom, roi_pan);
  ASSERT_TRUE(anchored_uv_with_roi.has_value());
  EXPECT_GT(std::abs(roi_pan.x() - reference_pan.x()), 5.0e-2);
  EXPECT_GT(std::abs(anchored_uv_with_roi->x() - anchor_uv_before->x()),
            std::abs(anchored_uv_with_reference->x() - anchor_uv_before->x()));
  EXPECT_NEAR(anchored_uv_with_roi->y(), anchor_uv_before->y(), 1.0e-5);
}

TEST(EditViewerLogicTests, CropAspectChangesRequirePanToBeReclampedToNewReference) {
  const ViewportWidgetInfo widget_info{800, 600, 1.0f};
  const ViewportImageInfo  pre_crop_image{4000, 3000};
  const ViewportImageInfo  post_crop_image{3000, 3000};
  constexpr float          zoom         = 2.0f;

  const QVector2D          pre_crop_pan = ViewportMapper::ClampPanForZoom(
      widget_info, pre_crop_image, zoom, QVector2D(0.7f, 0.0f),
      ViewTransformController::kMinInteractiveZoom, ViewTransformController::kMaxInteractiveZoom);
  const QVector2D post_crop_pan = ViewportMapper::ClampPanForZoom(
      widget_info, post_crop_image, zoom, pre_crop_pan,
      ViewTransformController::kMinInteractiveZoom, ViewTransformController::kMaxInteractiveZoom);

  EXPECT_GT(pre_crop_pan.x(), post_crop_pan.x());
  EXPECT_FLOAT_EQ(post_crop_pan.y(), 0.0f);
  EXPECT_LE(post_crop_pan.x(), 0.5f);
}

TEST(EditViewerLogicTests, CropInteractionControllerCreatesAndFinalizesCropRect) {
  ViewerState               state;
  CropInteractionController controller;
  auto                      crop_state = state.GetCropOverlay();
  crop_state.tool_enabled              = true;
  crop_state.overlay_visible           = true;
  crop_state.rect                      = QRectF(0.25, 0.25, 0.5, 0.5);
  state.SetCropOverlayState(crop_state);

  const QPointF start_point = WidgetPointForUv(QPointF(0.1, 0.1));
  const auto    press       = controller.HandlePress(state, kWidgetInfo, kImageInfo, start_point);
  EXPECT_TRUE(press.consumed);
  EXPECT_TRUE(press.rect_changed.has_value());

  const QPointF end_point = WidgetPointForUv(QPointF(0.4, 0.45));
  const auto    move =
      controller.HandleMove(state, kWidgetInfo, kImageInfo, Qt::LeftButton, end_point);
  EXPECT_TRUE(move.consumed);
  ASSERT_TRUE(move.rect_changed.has_value());
  EXPECT_NEAR(move.rect_changed->x(), 0.1, 1e-3);
  EXPECT_NEAR(move.rect_changed->y(), 0.1, 1e-3);
  EXPECT_GT(move.rect_changed->width(), 0.25);
  EXPECT_GT(move.rect_changed->height(), 0.3);

  const auto release = controller.HandleRelease(state);
  EXPECT_TRUE(release.consumed);
  EXPECT_TRUE(release.rect_is_final);
}

TEST(EditViewerLogicTests, CropInteractionControllerDoubleClickResetsCropAndRotation) {
  ViewerState               state;
  CropInteractionController controller;
  auto                      crop_state = state.GetCropOverlay();
  crop_state.tool_enabled              = true;
  crop_state.overlay_visible           = true;
  crop_state.metric_aspect             = 4.0f / 3.0f;
  crop_state.rect                      = QRectF(0.25, 0.25, 0.5, 0.5);
  crop_state.rotation_degrees          = -180.0f;
  state.SetCropOverlayState(crop_state);

  const auto result = controller.HandleDoubleClick(state);
  EXPECT_TRUE(result.consumed);
  ASSERT_TRUE(result.rect_changed.has_value());
  ASSERT_TRUE(result.rotation_changed.has_value());
  EXPECT_FLOAT_EQ(*result.rotation_changed, 0.0f);
  EXPECT_NEAR(result.rect_changed->x(), 0.0, 1e-4);
  EXPECT_NEAR(result.rect_changed->y(), 0.0, 1e-4);
  EXPECT_NEAR(result.rect_changed->width(), 1.0, 1e-4);
  EXPECT_NEAR(result.rect_changed->height(), 1.0, 1e-4);

  const auto updated_state = state.GetCropOverlay();
  EXPECT_FLOAT_EQ(updated_state.rotation_degrees, 0.0f);
  EXPECT_FALSE(updated_state.aspect_locked);
}

TEST(EditViewerLogicTests, CropGeometryHitTestPrefersCornersOverEdges) {
  const QRectF rect       = QRectF(0.2, 0.2, 0.4, 0.4);
  const auto   corners_uv = CropGeometry::RotatedCropCornersUv(
      rect, 0.0f, CropGeometry::SafeAspect(kImageInfo.image_width, kImageInfo.image_height));
  std::array<QPointF, 4> corners_widget{};
  for (size_t i = 0; i < corners_uv.size(); ++i) {
    const auto point = ViewportMapper::ImageUvToWidgetPoint(corners_uv[i], kWidgetInfo, kImageInfo,
                                                            1.0f, QVector2D(0.0f, 0.0f));
    ASSERT_TRUE(point.has_value());
    corners_widget[i] = *point;
  }

  const auto hit = CropGeometry::HitTestWidgetGeometry(corners_widget, corners_widget[0]);
  EXPECT_EQ(hit.corner_index, 0);
  EXPECT_EQ(hit.edge, CropEdge::None);
  EXPECT_FALSE(hit.rotate_handle_hit);
}

TEST(EditViewerLogicTests, OverlayGeometryBuildsCropCornersAndRotateHandleAtMultipleDpr) {
  for (const float dpr : {1.0f, 1.5f, 2.0f}) {
    EditViewerOverlaySnapshot snapshot;
    snapshot.widget_info = {800, 600, dpr};
    snapshot.image_info = kImageInfo;
    snapshot.viewer_state.view_transform = {1.0f, QVector2D(0.0f, 0.0f)};
    snapshot.viewer_state.crop_overlay.overlay_visible = true;
    snapshot.viewer_state.crop_overlay.tool_enabled = true;
    snapshot.viewer_state.crop_overlay.rect = QRectF(0.2, 0.2, 0.5, 0.5);
    snapshot.viewer_state.crop_overlay.metric_aspect =
        CropGeometry::SafeAspect(kImageInfo.image_width, kImageInfo.image_height);

    const auto geometry = EditViewerOverlayGeometry::Build(snapshot);
    ASSERT_TRUE(geometry.image_rect_valid) << "dpr=" << dpr;
    ASSERT_TRUE(geometry.crop_corners_valid) << "dpr=" << dpr;
    EXPECT_GT(geometry.image_rect.width(), 0.0);
    EXPECT_GT(geometry.image_rect.height(), 0.0);

    // Item/logical coordinates must not be multiplied by DPR (mapper divides back out).
    for (const auto& corner : geometry.crop_corners_widget) {
      EXPECT_GE(corner.x(), -1.0);
      EXPECT_LE(corner.x(), 801.0);
      EXPECT_GE(corner.y(), -1.0);
      EXPECT_LE(corner.y(), 601.0);
    }
    EXPECT_NE(geometry.rotate_stem_widget, geometry.rotate_handle_widget);

    // Round-trip a corner through UV to prove coordinate space stability at this DPR.
    const auto uv = ViewportMapper::WidgetPointToImageUv(
        geometry.crop_corners_widget[0], snapshot.widget_info, snapshot.image_info, 1.0f,
        QVector2D(0.0f, 0.0f));
    ASSERT_TRUE(uv.has_value());
    EXPECT_NEAR(uv->x(), 0.2, 2.0e-3);
    EXPECT_NEAR(uv->y(), 0.2, 2.0e-3);
  }
}

TEST(EditViewerLogicTests, OverlayGeometryGoldenMatchesLandscapePortraitSquareAndOddViewports) {
  struct Case {
    const char* name;
    int width;
    int height;
    int image_w;
    int image_h;
    QRectF crop;
  };
  const Case cases[] = {
      {"landscape", 960, 540, 4000, 2250, QRectF(0.1, 0.15, 0.7, 0.6)},
      {"portrait", 540, 960, 2250, 4000, QRectF(0.2, 0.1, 0.55, 0.7)},
      {"square", 700, 700, 3000, 3000, QRectF(0.25, 0.25, 0.5, 0.5)},
      {"odd", 801, 599, 4001, 3001, QRectF(0.05, 0.08, 0.81, 0.77)},
  };

  for (const auto& c : cases) {
    EditViewerOverlaySnapshot snapshot;
    snapshot.widget_info = {c.width, c.height, 1.0f};
    snapshot.image_info = {c.image_w, c.image_h};
    snapshot.viewer_state.view_transform = {1.0f, QVector2D(0.0f, 0.0f)};
    snapshot.viewer_state.crop_overlay.overlay_visible = true;
    snapshot.viewer_state.crop_overlay.tool_enabled = true;
    snapshot.viewer_state.crop_overlay.rect = c.crop;
    snapshot.viewer_state.crop_overlay.metric_aspect =
        CropGeometry::SafeAspect(c.image_w, c.image_h);

    const auto geometry = EditViewerOverlayGeometry::Build(snapshot);
    ASSERT_TRUE(geometry.image_rect_valid) << c.name;
    ASSERT_TRUE(geometry.crop_corners_valid) << c.name;

    // Golden: image UV corners map inside the letterboxed image rect, and
    // crop corners stay inside that rect for an unrotated full-frame view.
    for (const auto& corner : geometry.crop_corners_widget) {
      EXPECT_GE(corner.x(), geometry.image_rect.left() - 1.0) << c.name;
      EXPECT_LE(corner.x(), geometry.image_rect.right() + 1.0) << c.name;
      EXPECT_GE(corner.y(), geometry.image_rect.top() - 1.0) << c.name;
      EXPECT_LE(corner.y(), geometry.image_rect.bottom() + 1.0) << c.name;
    }

    // Rotate handle is offset outside the crop edge.
    const auto center = CropGeometry::CropCenterWidgetPoint(geometry.crop_corners_widget);
    const auto stem_to_handle = geometry.rotate_handle_widget - geometry.rotate_stem_widget;
    EXPECT_GT(QPointF::dotProduct(stem_to_handle, stem_to_handle), 1.0) << c.name;
    EXPECT_NE(geometry.rotate_handle_widget, center) << c.name;
  }
}

TEST(EditViewerLogicTests, OverlayGeometryMapsDetailRoiBoundsInSourceImageUv) {
  EditViewerOverlaySnapshot snapshot;
  snapshot.widget_info = kWidgetInfo;
  snapshot.image_info = kImageInfo;
  snapshot.viewer_state.view_transform = {2.0f, QVector2D(0.1f, -0.05f)};
  snapshot.detail_roi_visible = true;
  snapshot.detail_roi_uv = QRectF(0.3, 0.25, 0.2, 0.15);

  const auto geometry = EditViewerOverlayGeometry::Build(snapshot);
  ASSERT_TRUE(geometry.detail_roi_valid);

  const auto tl = ViewportMapper::WidgetPointToImageUv(
      geometry.detail_roi_corners_widget[0], snapshot.widget_info, snapshot.image_info, 2.0f,
      QVector2D(0.1f, -0.05f));
  ASSERT_TRUE(tl.has_value());
  EXPECT_NEAR(tl->x(), 0.3, 2.0e-3);
  EXPECT_NEAR(tl->y(), 0.25, 2.0e-3);
}

}  // namespace alcedo
