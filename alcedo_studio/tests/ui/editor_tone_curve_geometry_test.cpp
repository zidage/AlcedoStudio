//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6B pure geometry tests for the tone-curve scene-graph editor. Verifies
// plot padding, normalized↔widget mapping, hit testing, and scene-geometry
// construction without creating a QQuickWindow or GPU context.

#include <gtest/gtest.h>

#include <QPointF>
#include <QRectF>
#include <cmath>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_tone_curve_item.hpp"
#include "ui/alcedo_main/editor_support/modules/curve.hpp"

namespace alcedo::ui::test {
namespace {

TEST(EditorToneCurveGeometryTest, PlotRectUsesLegacyPadding) {
  const QRectF plot = ToneCurvePlotRect(320, 240);
  EXPECT_NEAR(plot.left(), 22.0, 1e-6);
  EXPECT_NEAR(plot.top(), 14.0, 1e-6);
  EXPECT_NEAR(plot.width(), 320.0 - 22.0 - 12.0, 1e-6);
  EXPECT_NEAR(plot.height(), 240.0 - 14.0 - 24.0, 1e-6);
}

TEST(EditorToneCurveGeometryTest, NormalizedWidgetRoundTrip) {
  const QRectF  plot = ToneCurvePlotRect(320, 240);
  const QPointF n(0.25, 0.75);
  const QPointF w    = ToneCurveToWidgetPoint(n, plot);
  const QPointF back = ToneCurveToNormalizedPoint(w, plot);
  EXPECT_NEAR(back.x(), n.x(), 1e-6);
  EXPECT_NEAR(back.y(), n.y(), 1e-6);

  // Y is inverted: higher normalized y maps toward the top of the plot.
  const QPointF high = ToneCurveToWidgetPoint(QPointF(0.5, 1.0), plot);
  const QPointF low  = ToneCurveToWidgetPoint(QPointF(0.5, 0.0), plot);
  EXPECT_LT(high.y(), low.y());
}

TEST(EditorToneCurveGeometryTest, HitTestPrefersNearestHandle) {
  const QRectF               plot   = ToneCurvePlotRect(320, 240);
  const std::vector<QPointF> points = {QPointF(0.0, 0.0), QPointF(0.5, 0.5), QPointF(1.0, 1.0)};
  const QPointF              mid    = ToneCurveToWidgetPoint(points[1], plot);
  EXPECT_EQ(ToneCurveHitTestPoint(mid, points, plot), 1);
  EXPECT_EQ(ToneCurveHitTestPoint(mid + QPointF(3.0, -2.0), points, plot), 1);
  EXPECT_EQ(ToneCurveHitTestPoint(QPointF(0.0, 0.0), points, plot), -1);
}

TEST(EditorToneCurveGeometryTest, SceneGeometrySamplesHermiteAndHandles) {
  const std::vector<QPointF> points = {QPointF(0.0, 0.0), QPointF(0.4, 0.6), QPointF(1.0, 1.0)};
  const auto                 g = BuildToneCurveSceneGeometry(points, 320, 240, /*active_index=*/1,
                                                             /*sample_count=*/64);
  EXPECT_EQ(g.handles.size(), 3u);
  EXPECT_EQ(g.active_index, 1);
  EXPECT_EQ(static_cast<int>(g.curve_samples.size()), 65);
  EXPECT_FALSE(g.grid_segments.empty());
  EXPECT_EQ(g.diagonal.size(), 2u);

  // Handle centers must match the normalized→widget mapping.
  for (size_t i = 0; i < points.size(); ++i) {
    const QPointF expected = ToneCurveToWidgetPoint(points[i], g.plot_rect);
    EXPECT_NEAR(g.handles[i].x(), expected.x(), 1e-6);
    EXPECT_NEAR(g.handles[i].y(), expected.y(), 1e-6);
  }

  // First and last curve samples sit on the endpoint handles.
  EXPECT_NEAR(g.curve_samples.front().x(), g.handles.front().x(), 1.0);
  EXPECT_NEAR(g.curve_samples.back().x(), g.handles.back().x(), 1.0);
}

TEST(EditorToneCurveGeometryTest, LiftedBlackEndpointRaisesNearBlackOutput) {
  const std::vector<QPointF> lifted = {QPointF(0.0, 0.2), QPointF(0.25, 0.25), QPointF(0.75, 0.75),
                                       QPointF(1.0, 1.0)};
  const auto                 cache  = curve::BuildCurveHermiteCache(lifted);
  EXPECT_GT(curve::EvaluateCurveHermite(0.10f, lifted, cache), 0.10f);
}

}  // namespace
}  // namespace alcedo::ui::test
