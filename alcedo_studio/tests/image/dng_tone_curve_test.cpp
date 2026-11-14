// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// DngToneCurve against a line-by-line port of the DNG SDK 1.4 `dng_spline_solver`,
// `dng_1d_table`, and `RefBaselineRGBTone` (dng_spline.cpp, dng_1d_table.cpp, dng_reference.cpp),
// and against the spline properties that the SDK states: through every point, C1 and C2
// continuous, second derivative zero at both ends (dcp_profile_tone_curve_study.md, phase T1).

#include "image/dng_tone_curve.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace alcedo {
namespace {

/// The SDK classes as the SDK writes them, kept apart from the production code.
namespace sdk {

class SplineSolver {
 public:
  void Add(double x, double y) {
    x_.push_back(x);
    y_.push_back(y);
  }

  void Solve() {
    const int count = static_cast<int>(x_.size());
    const int start = 0;
    const int end   = count;
    double    a     = x_[start + 1] - x_[start];
    double    b     = (y_[start + 1] - y_[start]) / a;
    s_.resize(count);
    s_[start] = b;
    int j;
    for (j = start + 2; j < end; ++j) {
      const double c = x_[j] - x_[j - 1];
      const double d = (y_[j] - y_[j - 1]) / c;
      s_[j - 1]      = (b * c + d * a) / (a + c);
      a              = c;
      b              = d;
    }
    s_[end - 1] = 2.0 * b - s_[end - 2];
    s_[start]   = 2.0 * s_[start] - s_[start + 1];
    if ((end - start) > 2) {
      std::vector<double> e(count);
      std::vector<double> f(count);
      std::vector<double> g(count);
      f[start]   = 0.5;
      e[end - 1] = 0.5;
      g[start]   = 0.75 * (s_[start] + s_[start + 1]);
      g[end - 1] = 0.75 * (s_[end - 2] + s_[end - 1]);
      for (j = start + 1; j < end - 1; ++j) {
        a    = (x_[j + 1] - x_[j - 1]) * 2.0;
        e[j] = (x_[j + 1] - x_[j]) / a;
        f[j] = (x_[j] - x_[j - 1]) / a;
        g[j] = 1.5 * s_[j];
      }
      for (j = start + 1; j < end; ++j) {
        a = 1.0 - f[j - 1] * e[j];
        if (j != end - 1) f[j] /= a;
        g[j] = (g[j] - g[j - 1] * e[j]) / a;
      }
      for (j = end - 2; j >= start; --j) g[j] = g[j] - f[j] * g[j + 1];
      for (j = start; j < end; ++j) s_[j] = g[j];
    }
  }

  [[nodiscard]] auto Evaluate(double x) const -> double {
    const int count = static_cast<int>(x_.size());
    if (x <= x_[0]) return y_[0];
    if (x >= x_[count - 1]) return y_[count - 1];
    int lower = 1;
    int upper = count - 1;
    while (upper > lower) {
      const int mid = (lower + upper) >> 1;
      if (x == x_[mid]) return y_[mid];
      if (x > x_[mid]) {
        lower = mid + 1;
      } else {
        upper = mid;
      }
    }
    const int    j  = lower;
    const double aa = x_[j] - x_[j - 1];
    const double bb = (x - x_[j - 1]) / aa;
    const double cc = (x_[j] - x) / aa;
    return ((y_[j - 1] * (2.0 - cc + bb) + (s_[j - 1] * aa * bb)) * (cc * cc)) +
           ((y_[j] * (2.0 - bb + cc) - (s_[j] * aa * cc)) * (bb * bb));
  }

 private:
  std::vector<double> x_;
  std::vector<double> y_;
  std::vector<double> s_;
};

constexpr std::uint32_t kTableSize = 4096;

/// dng_1d_table::Initialize without subsampling, with the guard entry.
class Table1d {
 public:
  explicit Table1d(const SplineSolver& function) : table_(kTableSize + 2) {
    for (std::uint32_t j = 0; j <= kTableSize; j++) {
      const double x = j * (1.0 / static_cast<double>(kTableSize));
      table_[j]      = static_cast<float>(function.Evaluate(x));
    }
    table_[kTableSize + 1] = table_[kTableSize];
  }

  [[nodiscard]] auto Interpolate(float x) const -> float {
    const float y     = x * static_cast<float>(kTableSize);
    const auto  index = static_cast<std::int32_t>(y);
    const float z     = static_cast<float>(index);
    const float fract = y - z;
    return table_[index] * (1.0f - fract) + table_[index + 1] * fract;
  }

  [[nodiscard]] auto Entry(std::size_t j) const -> float { return table_[j]; }

 private:
  std::vector<float> table_;
};

/// RefBaselineRGBTone for one pixel.
auto BaselineRgbTone(const Table1d& table, float r, float g, float b) -> std::array<float, 3> {
  float rr;
  float gg;
  float bb;
#define RGB_TONE(r, g, b, rr, gg, bb)          \
  {                                            \
    rr = table.Interpolate(r);                 \
    bb = table.Interpolate(b);                 \
    gg = bb + ((rr - bb) * (g - b) / (r - b)); \
  }
  if (r >= g) {
    if (g > b) {
      RGB_TONE(r, g, b, rr, gg, bb);
    } else if (b > r) {
      RGB_TONE(b, r, g, bb, rr, gg);
    } else if (b > g) {
      RGB_TONE(r, b, g, rr, bb, gg);
    } else {
      rr = table.Interpolate(r);
      gg = table.Interpolate(g);
      bb = gg;
    }
  } else {
    if (r >= b) {
      RGB_TONE(g, r, b, gg, rr, bb);
    } else if (b > g) {
      RGB_TONE(b, g, r, bb, gg, rr);
    } else {
      RGB_TONE(g, b, r, gg, bb, rr);
    }
  }
#undef RGB_TONE
  return {rr, gg, bb};
}

}  // namespace sdk

auto SdkSolver(const std::vector<DngToneCurvePoint>& points) -> sdk::SplineSolver {
  sdk::SplineSolver solver;
  for (const auto& point : points) solver.Add(point.x, point.y);
  solver.Solve();
  return solver;
}

/// A camera-like curve: 128 points of a power curve with a shoulder, through 0.444 at scene 0.18,
/// near the median of the user folder (study section 3.1). The points are dense near black.
auto CameraLikePoints() -> std::vector<DngToneCurvePoint> {
  std::vector<DngToneCurvePoint> points;
  constexpr int                  kCount = 128;
  const auto                     shape  = [](double x) {
    return std::min(1.0, std::pow(x, 0.62) * (1.0 + 0.35 * (1.0 - x)));
  };
  for (int i = 0; i < kCount; ++i) {
    const double x = std::pow(static_cast<double>(i) / (kCount - 1), 1.6);
    points.push_back({static_cast<float>(x), static_cast<float>(shape(x))});
  }
  return points;
}

/// The test curves: the camera-like curve, a short S curve, a curve that reaches 1 before x = 1
/// with a falling segment, and the two- and three-point cases.
auto TestCurves() -> std::vector<std::vector<DngToneCurvePoint>> {
  return {CameraLikePoints(),
          {{0.0f, 0.0f}, {0.1f, 0.06f}, {0.25f, 0.3f}, {0.5f, 0.68f}, {0.75f, 0.9f}, {1.0f, 1.0f}},
          {{0.0f, 0.0f}, {0.3f, 0.5f}, {0.6f, 0.82f}, {0.62f, 0.815f}, {0.97f, 1.0f}},
          {{0.0f, 0.0f}, {1.0f, 1.0f}},
          {{0.0f, 0.05f}, {0.4f, 0.6f}, {1.0f, 0.95f}}};
}

TEST(DngToneCurveTest, SplineAndTableMatchTheDngSdkWithin1e6) {
  for (const auto& points : TestCurves()) {
    const auto         curve  = MakeDngToneCurve(points);
    const auto         solver = SdkSolver(points);
    const sdk::Table1d table(solver);
    for (int i = 0; i <= 100000; ++i) {
      const double x = static_cast<double>(i) / 100000.0;
      ASSERT_NEAR(curve->Evaluate(x), solver.Evaluate(x), 1e-6) << "x " << x;
    }
    for (std::size_t j = 0; j < kDngToneTableEntries; ++j) {
      ASSERT_NEAR(curve->table[j], table.Entry(j), 1e-6) << "entry " << j;
    }
    for (int i = 0; i <= 20000; ++i) {
      const float x = static_cast<float>(i) / 20000.0f;
      ASSERT_NEAR(curve->Interpolate(x), table.Interpolate(x), 1e-6) << "x " << x;
    }
  }
}

/// Smallest distance between two neighbouring points.
auto MinSpacing(const std::vector<DngToneCurvePoint>& points) -> double {
  double spacing = 1.0;
  for (std::size_t i = 1; i < points.size(); ++i) {
    spacing = std::min(spacing, static_cast<double>(points[i].x) - points[i - 1].x);
  }
  return spacing;
}

TEST(DngToneCurveTest, SplinePassesThroughEveryPointWithContinuousFirstAndSecondDerivatives) {
  constexpr double kStep = 1e-5;
  for (const auto& points : TestCurves()) {
    const auto curve = MakeDngToneCurve(points);
    for (std::size_t i = 0; i < points.size(); ++i) {
      EXPECT_DOUBLE_EQ(curve->Evaluate(points[i].x), points[i].y) << "point " << i;
    }
    // Difference quotients resolve the derivatives only where the points lie far apart compared
    // with the step; the dense camera-like curve is checked against the SDK port alone.
    if (MinSpacing(points) < 0.01) continue;
    // One-sided difference quotients from the two segments that meet at an inner point.
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
      const double x       = points[i].x;
      const double y       = points[i].y;
      const double left    = curve->Evaluate(x - kStep);
      const double left2   = curve->Evaluate(x - 2 * kStep);
      const double right   = curve->Evaluate(x + kStep);
      const double right2  = curve->Evaluate(x + 2 * kStep);
      const double slope_l = (y - left) / kStep;
      const double slope_r = (right - y) / kStep;
      EXPECT_NEAR(slope_l, curve->slopes[i], 1e-3 * (1.0 + std::abs(curve->slopes[i]))) << i;
      EXPECT_NEAR(slope_r, curve->slopes[i], 1e-3 * (1.0 + std::abs(curve->slopes[i]))) << i;
      const double second_l = (y - 2 * left + left2) / (kStep * kStep);
      const double second_r = (right2 - 2 * right + y) / (kStep * kStep);
      EXPECT_NEAR(second_l, second_r, 2e-2 * (1.0 + std::abs(second_l))) << i;
    }
  }
}

TEST(DngToneCurveTest, SplineHasZeroSecondDerivativeAtBothEnds) {
  constexpr double kStep = 1e-5;
  for (const auto& points : TestCurves()) {
    if (points.size() < 3 || MinSpacing(points) < 0.01) continue;
    const auto   curve = MakeDngToneCurve(points);
    const double x0    = points.front().x;
    const double x1    = points.back().x;
    const double start =
        (curve->Evaluate(x0 + 2 * kStep) - 2 * curve->Evaluate(x0 + kStep) + curve->Evaluate(x0)) /
        (kStep * kStep);
    const double end =
        (curve->Evaluate(x1) - 2 * curve->Evaluate(x1 - kStep) + curve->Evaluate(x1 - 2 * kStep)) /
        (kStep * kStep);
    // The quotient measures the second derivative a step inside the end; the third derivative
    // times 1.5e-5 bounds the difference.
    EXPECT_NEAR(start, 0.0, 0.05) << points.size();
    EXPECT_NEAR(end, 0.0, 0.05) << points.size();
  }
}

TEST(DngToneCurveTest, TwoPointCurveIsTheLineThroughThePoints) {
  const auto curve = MakeDngToneCurve({{0.1f, 0.2f}, {0.9f, 0.6f}});
  for (double x = 0.1; x <= 0.9; x += 0.01) {
    EXPECT_NEAR(curve->Evaluate(x), 0.2 + (x - 0.1) * 0.5, 1e-7) << x;
  }
}

TEST(DngToneCurveTest, ValuesOutsideThePointRangeHoldTheEndValues) {
  const auto curve = MakeDngToneCurve({{0.1f, 0.05f}, {0.5f, 0.7f}, {0.96f, 1.0f}});
  EXPECT_DOUBLE_EQ(curve->Evaluate(0.0), 0.05f);
  EXPECT_DOUBLE_EQ(curve->Evaluate(0.99), 1.0);
  EXPECT_FLOAT_EQ(curve->table[0], 0.05f);
  EXPECT_FLOAT_EQ(curve->table[kDngToneTableIntervals], 1.0f);
  EXPECT_FLOAT_EQ(curve->Interpolate(1.0f), 1.0f);
  EXPECT_FLOAT_EQ(curve->Interpolate(1.5f), 1.0f);
  EXPECT_FLOAT_EQ(curve->Interpolate(-0.5f), 0.05f);
}

TEST(DngToneCurveTest, BaselineRgbToneMatchesTheSdkInEveryChannelOrder) {
  const auto                              points = CameraLikePoints();
  const auto                              curve  = MakeDngToneCurve(points);
  const auto                              solver = SdkSolver(points);
  const sdk::Table1d                      table(solver);
  const std::vector<std::array<float, 3>> pixels{
      {0.6f, 0.3f, 0.1f}, {0.3f, 0.1f, 0.6f}, {0.6f, 0.1f, 0.3f},   {0.6f, 0.2f, 0.2f},
      {0.3f, 0.6f, 0.1f}, {0.1f, 0.3f, 0.6f}, {0.1f, 0.6f, 0.3f},   {0.25f, 0.25f, 0.25f},
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.5f, 0.0f}, {0.18f, 0.18f, 0.5f}, {0.02f, 0.9f, 0.9f}};
  for (const auto& pixel : pixels) {
    std::array<float, 3> actual = pixel;
    ApplyDngBaselineRgbTone(*curve, actual);
    const auto expected = sdk::BaselineRgbTone(table, pixel[0], pixel[1], pixel[2]);
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(actual[c], expected[c], 1e-6)
          << pixel[0] << ' ' << pixel[1] << ' ' << pixel[2] << " channel " << c;
    }
  }
}

TEST(DngToneCurveTest, BaselineRgbToneKeepsTheRelativePositionOfTheMiddleChannel) {
  const auto           curve = MakeDngToneCurve(CameraLikePoints());
  std::array<float, 3> rgb{0.5f, 0.2f, 0.1f};
  ApplyDngBaselineRgbTone(*curve, rgb);
  EXPECT_FLOAT_EQ(rgb[0], curve->Interpolate(0.5f));
  EXPECT_FLOAT_EQ(rgb[2], curve->Interpolate(0.1f));
  EXPECT_NEAR((rgb[1] - rgb[2]) / (rgb[0] - rgb[2]), (0.2f - 0.1f) / (0.5f - 0.1f), 1e-6);
  std::array<float, 3> neutral{0.18f, 0.18f, 0.18f};
  ApplyDngBaselineRgbTone(*curve, neutral);
  for (float value : neutral) EXPECT_FLOAT_EQ(value, curve->Interpolate(0.18f));
}

TEST(DngToneCurveTest, FingerprintDependsOnlyOnThePoints) {
  const auto a = MakeDngToneCurve({{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}});
  const auto b = MakeDngToneCurve({{0.0f, 0.0f}, {0.5f, 0.6f}, {1.0f, 1.0f}});
  const auto c = MakeDngToneCurve({{0.0f, 0.0f}, {0.5f, 0.61f}, {1.0f, 1.0f}});
  EXPECT_EQ(a->fingerprint, b->fingerprint);
  EXPECT_NE(a->fingerprint, c->fingerprint);
  EXPECT_NE(a->fingerprint, 0u);
}

auto MakeError(std::vector<DngToneCurvePoint> points) -> std::string {
  try {
    (void)MakeDngToneCurve(std::move(points));
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  ADD_FAILURE() << "MakeDngToneCurve did not throw";
  return {};
}

TEST(DngToneCurveTest, InvalidPointsFailWithTheConditionAndThePointIndex) {
  EXPECT_NE(MakeError({{0.0f, 0.0f}}).find("1 points, expected at least 2"), std::string::npos);
  EXPECT_NE(MakeError({{0.0f, 0.0f}, {0.0f, 1.0f}}).find("point 1: x does not rise"),
            std::string::npos);
  EXPECT_NE(MakeError({{0.0f, 0.0f}, {1.0f, 1.01f}}).find("point 1: y is outside [0, 1]"),
            std::string::npos);
  EXPECT_NE(MakeError({{0.0f, 0.0f}, {1.5f, 1.0f}}).find("point 1: x is outside [0, 1]"),
            std::string::npos);
  EXPECT_NE(MakeError({{0.0f, std::nanf("")}, {1.0f, 1.0f}}).find("point 0: a value is not"),
            std::string::npos);
  EXPECT_NE(
      MakeError(std::vector<DngToneCurvePoint>(kMaxDngToneCurvePoints + 1)).find("above the limit"),
      std::string::npos);
}

}  // namespace
}  // namespace alcedo
