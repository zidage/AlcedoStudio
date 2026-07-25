//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6D pure geometry tests for the CDL trackball disc. GPU-free: disc
// padding, normalized ↔ widget round-trip, and clamp-to-unit-circle behavior
// match the legacy CdlTrackballDiscWidget coordinate conventions.

#include "ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_wheel.hpp"

#include <QPointF>
#include <QRectF>

#include <gtest/gtest.h>

#include <cmath>

namespace alcedo::ui::test {
namespace {

auto Nearly(double a, double b, double eps = 1e-6) -> bool { return std::abs(a - b) <= eps; }

}  // namespace

TEST(EditorCdlTrackballGeometryTest, DiscRectIsCenteredSquareWithInset) {
  const QRectF disc = CdlTrackballDiscRect(160.0, 140.0);
  EXPECT_NEAR(disc.width(), disc.height(), 1e-9);
  EXPECT_NEAR(disc.width(), 140.0 - 6.0, 1e-6);  // min side - 2*inset
  EXPECT_NEAR(disc.center().x(), 80.0, 1e-6);
  EXPECT_NEAR(disc.center().y(), 70.0, 1e-6);
}

TEST(EditorCdlTrackballGeometryTest, NormalizedWidgetRoundTripPreservesInteriorPoints) {
  const QRectF disc = CdlTrackballDiscRect(180.0, 180.0);
  const QPointF samples[] = {
      QPointF(0.0, 0.0),
      QPointF(0.5, 0.0),
      QPointF(0.0, -0.75),
      QPointF(-0.3, 0.4),
  };
  for (const QPointF& n : samples) {
    const QPointF w = CdlTrackballToWidgetPoint(n, disc);
    const QPointF back = CdlTrackballToNormalizedPoint(w, disc);
    EXPECT_TRUE(Nearly(back.x(), n.x())) << n.x();
    EXPECT_TRUE(Nearly(back.y(), n.y())) << n.y();
  }
}

TEST(EditorCdlTrackballGeometryTest, OutsidePointsClampToUnitCircle) {
  const QRectF  disc = CdlTrackballDiscRect(120.0, 120.0);
  const QPointF far  = CdlTrackballToNormalizedPoint(QPointF(0.0, 0.0), disc);  // top-left corner
  const double  r    = std::sqrt(far.x() * far.x() + far.y() * far.y());
  EXPECT_LE(r, 1.0 + 1e-6);

  const QPointF clamped = color_wheel::ClampDiscPoint(QPointF(2.0, 0.0));
  EXPECT_NEAR(clamped.x(), 1.0, 1e-6);
  EXPECT_NEAR(clamped.y(), 0.0, 1e-6);
}

TEST(EditorCdlTrackballGeometryTest, DiscToCdlDeltaIsZeroAtCenter) {
  const auto delta = color_wheel::DiscToCdlDelta(QPointF(0.0, 0.0), color_wheel::kStrengthDefault);
  EXPECT_NEAR(delta[0], 0.0f, 1e-5f);
  EXPECT_NEAR(delta[1], 0.0f, 1e-5f);
  EXPECT_NEAR(delta[2], 0.0f, 1e-5f);
}

TEST(EditorCdlTrackballGeometryTest, DiscToCdlDeltaProducesChromaticOffsetOffCenter) {
  const auto delta =
      color_wheel::DiscToCdlDelta(QPointF(0.8, 0.0), color_wheel::kStrengthDefault);
  const float abs_sum = std::abs(delta[0]) + std::abs(delta[1]) + std::abs(delta[2]);
  EXPECT_GT(abs_sum, 1e-4f);
}

}  // namespace alcedo::ui::test
