//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/analytic_mask_edit.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace alcedo {
namespace {

TEST(AnalyticMaskEditTest, CenterOutRadiiStayPositiveAndUnswapped) {
  const Vector2 center{0.40f, 0.55f};
  const auto    left_up = RadialFromCenterOut(center, Vector2{0.25f, 0.70f});
  EXPECT_FLOAT_EQ(left_up.center_x, 0.40f);
  EXPECT_FLOAT_EQ(left_up.center_y, 0.55f);
  EXPECT_FLOAT_EQ(left_up.major_radius, 0.15f);
  EXPECT_FLOAT_EQ(left_up.minor_radius, 0.15f);
  EXPECT_FLOAT_EQ(left_up.rotation, 0.0f);
  EXPECT_FALSE(RadialCreationIsValid(RadialFromCenterOut(center, center)));

  RadialMaskSource tall;
  tall.center_x     = 0.50f;
  tall.center_y     = 0.50f;
  tall.major_radius = 0.20f;
  tall.minor_radius = 0.08f;
  const auto across = UpdateRadialMajorRadius(tall, Vector2{0.50f - 0.05f, 0.50f});
  ASSERT_TRUE(across.has_value());
  EXPECT_FLOAT_EQ(across->major_radius, 0.05f);
  EXPECT_FLOAT_EQ(across->minor_radius, 0.08f);
  EXPECT_EQ(across->center_x, tall.center_x);
}

TEST(AnalyticMaskEditTest, LinearEndpointsSetOriginNormalAndWidth) {
  const auto source = LinearFromEndpoints(Vector2{0.20f, 0.25f}, Vector2{0.80f, 0.25f});
  EXPECT_FLOAT_EQ(source.origin_x, 0.50f);
  EXPECT_FLOAT_EQ(source.origin_y, 0.25f);
  EXPECT_FLOAT_EQ(source.normal_x, 1.0f);
  EXPECT_FLOAT_EQ(source.normal_y, 0.0f);
  EXPECT_FLOAT_EQ(source.transition_distance, 0.60f);
  EXPECT_FLOAT_EQ(source.start_value, 1.0f);
  EXPECT_FLOAT_EQ(source.end_value, 0.0f);
  EXPECT_TRUE(LinearCreationIsValid(source));
  EXPECT_FALSE(LinearCreationIsValid(LinearFromEndpoints(Vector2{0.3f, 0.3f}, Vector2{0.3f, 0.3f})));
}

TEST(AnalyticMaskEditTest, RotationUnwrapsAcrossPi) {
  RadialMaskSource source;
  source.center_x     = 0.50f;
  source.center_y     = 0.50f;
  source.major_radius = 0.20f;
  source.minor_radius = 0.10f;
  float unwrapped     = 3.0f;
  source.rotation     = unwrapped;
  const float wrapped = -3.0f;
  const auto  next    = UpdateRadialRotation(
      source, Vector2{0.50f + std::cos(wrapped) * 0.2f, 0.50f + std::sin(wrapped) * 0.2f},
      unwrapped);
  ASSERT_TRUE(next.has_value());
  EXPECT_NEAR(unwrapped, 3.0f + (wrapped - 3.0f + 2.0f * 3.14159265358979323846f), 1.0e-4f);
  EXPECT_FLOAT_EQ(next->rotation, unwrapped);
  EXPECT_GT(unwrapped, 3.0f);
}

}  // namespace
}  // namespace alcedo
