//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/tone_curve_spline.hpp"

#include <cmath>
#include <stdexcept>

namespace alcedo::tone_curve {

auto FindToneCurvePointsError(std::span<const CurvePoint> points) -> std::optional<std::string> {
  if (points.size() < kMinControlPoints || points.size() > kMaxControlPoints) {
    return "Tone curve has " + std::to_string(points.size()) + " control points; a curve has " +
           std::to_string(kMinControlPoints) + " to " + std::to_string(kMaxControlPoints) +
           " points";
  }
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
      return "Tone curve point " + std::to_string(i) + " is not finite";
    }
    if (i > 0 && !(points[i].x > points[i - 1].x)) {
      return "Tone curve point " + std::to_string(i) + " x must be greater than point " +
             std::to_string(i - 1) + " x";
    }
  }
  return std::nullopt;
}

auto SolveToneCurveSpline(std::span<const CurvePoint> points) -> ToneCurveSpline {
  if (const auto error = FindToneCurvePointsError(points); error.has_value()) {
    throw std::invalid_argument(*error);
  }
  ToneCurveSpline spline;
  const auto      n = points.size();
  spline.count      = static_cast<std::uint32_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    spline.x[i] = points[i].x;
    spline.y[i] = points[i].y;
  }

  std::array<float, kMaxControlPoints> width{};
  std::array<float, kMaxControlPoints> secant{};
  for (std::size_t i = 0; i + 1 < n; ++i) {
    width[i]  = spline.x[i + 1] - spline.x[i];
    secant[i] = (spline.y[i + 1] - spline.y[i]) / width[i];
  }

  spline.slope[0]     = secant[0];
  spline.slope[n - 1] = secant[n - 2];
  for (std::size_t i = 1; i + 1 < n; ++i) {
    if (secant[i - 1] * secant[i] <= 0.0f) {
      spline.slope[i] = 0.0f;
      continue;
    }
    const float w1  = 2.0f * width[i] + width[i - 1];
    const float w2  = width[i] + 2.0f * width[i - 1];
    spline.slope[i] = (w1 + w2) / (w1 / secant[i - 1] + w2 / secant[i]);
  }
  return spline;
}

auto EvaluateToneCurveSpline(const ToneCurveSpline& spline, float x) -> float {
  const std::uint32_t n = spline.count;
  if (n < kMinControlPoints) {
    return x;
  }
  if (x <= spline.x[0]) {
    return spline.y[0] + spline.slope[0] * (x - spline.x[0]);
  }
  if (x >= spline.x[n - 1]) {
    return spline.y[n - 1] + spline.slope[n - 1] * (x - spline.x[n - 1]);
  }
  std::uint32_t i = 0;
  while (i + 2 < n && x >= spline.x[i + 1]) {
    ++i;
  }
  // Hermite form y0 + t * dy + h * t * (1 - t) * ((1 - t) * (m0 - d) - t * (m1 - d)), with the
  // secant slope d = dy / h. The cubic term is exactly 0 on a straight segment.
  const float h  = spline.x[i + 1] - spline.x[i];
  const float t  = (x - spline.x[i]) / h;
  const float dy = spline.y[i + 1] - spline.y[i];
  const float d  = dy / h;
  const float u  = 1.0f - t;
  return spline.y[i] + t * dy +
         h * t * u * (u * (spline.slope[i] - d) - t * (spline.slope[i + 1] - d));
}

}  // namespace alcedo::tone_curve
