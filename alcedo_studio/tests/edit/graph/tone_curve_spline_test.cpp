//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/tone_curve_spline.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/operators/models/curve_model.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

using tone_curve::EvaluateToneCurveSpline;
using tone_curve::FindToneCurvePointsError;
using tone_curve::SolveToneCurveSpline;

/// Inputs from below the black end to above the white end of the curve axis.
auto SweepInputs() -> std::vector<float> {
  std::vector<float> inputs;
  for (int i = -200; i <= 300; ++i) {
    inputs.push_back(static_cast<float>(i) / 100.0f);
  }
  return inputs;
}

/// Independent reference: the panel evaluator before the shared spline (standard Hermite basis
/// h00, h10, h01, h11 and the same slope rule), extended along the end slopes.
auto ReferenceHermite(const std::vector<CurvePoint>& p, float x) -> float {
  const std::size_t  n = p.size();
  std::vector<float> h(n - 1);
  std::vector<float> delta(n - 1);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    h[i]     = p[i + 1].x - p[i].x;
    delta[i] = (p[i + 1].y - p[i].y) / h[i];
  }
  std::vector<float> m(n);
  m[0]     = delta[0];
  m[n - 1] = delta[n - 2];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    if (delta[i - 1] * delta[i] <= 0.0f) {
      m[i] = 0.0f;
      continue;
    }
    const float w1 = 2.0f * h[i] + h[i - 1];
    const float w2 = h[i] + 2.0f * h[i - 1];
    m[i]           = (w1 + w2) / ((w1 / delta[i - 1]) + (w2 / delta[i]));
  }
  if (x <= p.front().x) {
    return p.front().y + m.front() * (x - p.front().x);
  }
  if (x >= p.back().x) {
    return p.back().y + m.back() * (x - p.back().x);
  }
  std::size_t idx = n - 2;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (x < p[i + 1].x) {
      idx = i;
      break;
    }
  }
  const float t   = (x - p[idx].x) / h[idx];
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;
  return h00 * p[idx].y + h10 * h[idx] * m[idx] + h01 * p[idx + 1].y + h11 * h[idx] * m[idx + 1];
}

/// Sixteen points of a strong S curve with uneven spacing and one local extremum.
auto SixteenPointCurve() -> std::vector<CurvePoint> {
  return {{0.0f, 0.02f},  {0.05f, 0.03f}, {0.11f, 0.08f}, {0.18f, 0.12f},
          {0.24f, 0.21f}, {0.30f, 0.30f}, {0.36f, 0.42f}, {0.41f, 0.52f},
          {0.47f, 0.60f}, {0.53f, 0.66f}, {0.60f, 0.64f}, {0.68f, 0.70f},
          {0.76f, 0.80f}, {0.84f, 0.88f}, {0.92f, 0.93f}, {1.0f, 0.97f}};
}

TEST(ToneCurveSplineTest, IdentityPointsReturnTheInputExactlyAlsoOutsideTheUnitRange) {
  const auto spline = SolveToneCurveSpline(std::vector<CurvePoint>{{0.0f, 0.0f}, {1.0f, 1.0f}});
  for (const float x : SweepInputs()) {
    EXPECT_EQ(EvaluateToneCurveSpline(spline, x), x) << x;
  }
}

TEST(ToneCurveSplineTest, TwoPointCurveIsTheStraightSegmentAndItsExtension) {
  const std::vector<CurvePoint> points{{0.1f, 0.2f}, {0.9f, 0.7f}};
  const auto                    spline = SolveToneCurveSpline(points);
  const float                   dy     = points[1].y - points[0].y;
  const float                   dx     = points[1].x - points[0].x;
  for (const float x : SweepInputs()) {
    const float t        = (x - points[0].x) / dx;
    const float expected = points[0].y + t * dy;
    if (x > points[0].x && x < points[1].x) {
      // The straight segment has the same float value as the render before the spline.
      EXPECT_EQ(EvaluateToneCurveSpline(spline, x), expected) << x;
    } else {
      EXPECT_NEAR(EvaluateToneCurveSpline(spline, x), expected, 1.0e-6f) << x;
    }
  }
}

TEST(ToneCurveSplineTest, SplineMatchesThePreviousPanelHermiteForThreeAndSixteenPoints) {
  const std::vector<std::vector<CurvePoint>> curves{{{0.0f, 0.0f}, {0.4135f, 0.5f}, {1.0f, 1.0f}},
                                                    SixteenPointCurve()};
  for (const auto& points : curves) {
    const auto spline = SolveToneCurveSpline(points);
    for (const float x : SweepInputs()) {
      EXPECT_NEAR(EvaluateToneCurveSpline(spline, x), ReferenceHermite(points, x), 1.0e-6f)
          << points.size() << " points at " << x;
    }
  }
}

TEST(ToneCurveSplineTest, SplinePassesThroughEveryControlPoint) {
  const auto points = SixteenPointCurve();
  const auto spline = SolveToneCurveSpline(points);
  for (const auto& point : points) {
    EXPECT_NEAR(EvaluateToneCurveSpline(spline, point.x), point.y, 1.0e-6f) << point.x;
  }
}

TEST(ToneCurveSplineTest, SplineStaysBetweenTheValuesOfEachSegment) {
  const auto points = SixteenPointCurve();
  const auto spline = SolveToneCurveSpline(points);
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    const float low  = std::min(points[i].y, points[i + 1].y);
    const float high = std::max(points[i].y, points[i + 1].y);
    for (int step = 1; step < 64; ++step) {
      const float x =
          points[i].x + (points[i + 1].x - points[i].x) * static_cast<float>(step) / 64.0f;
      const float y = EvaluateToneCurveSpline(spline, x);
      EXPECT_GE(y, low - 1.0e-6f) << x;
      EXPECT_LE(y, high + 1.0e-6f) << x;
    }
  }
}

TEST(ToneCurveSplineTest, CurveContinuesAlongTheEndSlopeOutsideTheEndPoints) {
  const std::vector<CurvePoint> points{{0.2f, 0.1f}, {0.4135f, 0.5f}, {0.8f, 0.9f}};
  const auto                    spline = SolveToneCurveSpline(points);
  const float first_slope              = (points[1].y - points[0].y) / (points[1].x - points[0].x);
  const float last_slope               = (points[2].y - points[1].y) / (points[2].x - points[1].x);
  EXPECT_FLOAT_EQ(spline.slope[0], first_slope);
  EXPECT_FLOAT_EQ(spline.slope[2], last_slope);
  EXPECT_NEAR(EvaluateToneCurveSpline(spline, -0.5f), 0.1f + first_slope * -0.7f, 1.0e-6f);
  EXPECT_NEAR(EvaluateToneCurveSpline(spline, 1.5f), 0.9f + last_slope * 0.7f, 1.0e-6f);
}

TEST(ToneCurveSplineTest, InvalidPointsAreRejectedWithANamedError) {
  const auto error_of = [](const std::vector<CurvePoint>& points) {
    const auto error = FindToneCurvePointsError(points);
    EXPECT_TRUE(error.has_value());
    EXPECT_THROW((void)SolveToneCurveSpline(points), std::invalid_argument);
    return error.value_or("");
  };
  EXPECT_EQ(error_of({{0.5f, 0.5f}}),
            "Tone curve has 1 control points; a curve has 2 to 16 points");
  std::vector<CurvePoint> seventeen;
  for (int i = 0; i < 17; ++i) {
    const float x = static_cast<float>(i) / 16.0f;
    seventeen.push_back({x, x});
  }
  EXPECT_EQ(error_of(seventeen), "Tone curve has 17 control points; a curve has 2 to 16 points");
  EXPECT_EQ(error_of({{0.0f, 0.0f}, {0.5f, 0.4f}, {0.5f, 0.6f}, {1.0f, 1.0f}}),
            "Tone curve point 2 x must be greater than point 1 x");
  EXPECT_EQ(error_of({{0.0f, 0.0f}, {1.0f, std::numeric_limits<float>::quiet_NaN()}}),
            "Tone curve point 1 is not finite");

  seventeen.pop_back();
  EXPECT_FALSE(FindToneCurvePointsError(seventeen).has_value());
}

TEST(ToneCurveSplineTest, StoredCurveJsonReencodesToTheSameBytes) {
  // A curve as the panel stored it before the shared spline: the stored JSON does not change.
  const auto stored = nlohmann::json::parse(
      R"({"points":[{"x":0.0,"y":0.0},{"x":0.25,"y":0.18000000715255737},)"
      R"({"x":0.6000000238418579,"y":0.699999988079071},{"x":1.0,"y":1.0}]})");
  CurveModel model;
  model.LoadJson(stored);
  EXPECT_EQ(model.ToJson().dump(), stored.dump());
}

}  // namespace
}  // namespace alcedo
