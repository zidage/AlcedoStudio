//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Geometry overlay draft edits must not route CropRotate pipeline view changes.
/// Pipeline bake is owned by panel confirm (leave / Enter).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QPointF>
#include <QRectF>
#include <QSignalSpy>
#include <Qt>

#include "ui/editor_rhi/editor_interaction_controller.hpp"

namespace alcedo::editor_rhi {
namespace {

void ConfigureImage(EditorInteractionController& controller, int width, int height) {
  controller.setImageSize(width, height);
  controller.setRenderReferenceSize(width, height);
}

}  // namespace

TEST(EditorGeometryOverlayDraftRoutingTest,
     OverlayVisibleCropDraftDoesNotEmitViewChangeReported) {
  EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  ConfigureImage(controller, 400, 300);
  controller.setCropToolEnabled(true);
  controller.setCropOverlayVisible(true);

  QSignalSpy view_spy(&controller, &EditorInteractionController::viewChangeReported);
  ASSERT_TRUE(view_spy.isValid());
  QSignalSpy rect_spy(&controller, &EditorInteractionController::cropRectCommitted);
  ASSERT_TRUE(rect_spy.isValid());
  QSignalSpy rot_spy(&controller, &EditorInteractionController::cropRotationCommitted);
  ASSERT_TRUE(rot_spy.isValid());

  controller.setCropRectNormalized(QRectF(0.25, 0.25, 0.5, 0.5));
  EXPECT_NEAR(controller.cropRectNormalized().x(), 0.25, 1e-4);
  EXPECT_GE(rect_spy.count(), 1);
  EXPECT_TRUE(view_spy.empty());

  view_spy.clear();
  controller.setCropRotationDegrees(12.0f);
  EXPECT_NEAR(controller.cropRotationDegrees(), 12.0f, 1e-4f);
  EXPECT_GE(rot_spy.count(), 1);
  EXPECT_TRUE(view_spy.empty());

  view_spy.clear();
  rect_spy.clear();
  const QPointF start = controller.imageUvToItemPoint(0.1, 0.1);
  const QPointF end   = controller.imageUvToItemPoint(0.4, 0.45);
  ASSERT_FALSE(start.isNull());
  ASSERT_FALSE(end.isNull());
  controller.handlePress(start.x(), start.y(), static_cast<int>(Qt::LeftButton));
  controller.handleMove(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  controller.handleRelease(end.x(), end.y(), static_cast<int>(Qt::LeftButton));
  EXPECT_GE(rect_spy.count(), 1);
  EXPECT_TRUE(view_spy.empty()) << "crop-frame drag must stay pure UI while overlay is open";

  controller.setCropOverlayVisible(false);
  controller.setCropToolEnabled(false);
  view_spy.clear();
  controller.setCropRectNormalized(QRectF(0.1, 0.1, 0.8, 0.8));
  ASSERT_FALSE(view_spy.empty());
  EXPECT_EQ(view_spy.takeLast().at(0).toInt(),
            static_cast<int>(EditorInteractionController::ViewChangeKind::CropRotate));
}

}  // namespace alcedo::editor_rhi
