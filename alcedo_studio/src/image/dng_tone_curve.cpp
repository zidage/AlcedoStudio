// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dng_tone_curve.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {
namespace {

[[noreturn]] void FailPoint(std::size_t index, const std::string& reason) {
  throw std::runtime_error("ProfileToneCurve point " + std::to_string(index) + ": " + reason);
}

void ValidatePoints(const std::vector<DngToneCurvePoint>& points) {
  if (points.size() < 2) {
    throw std::runtime_error("ProfileToneCurve has " + std::to_string(points.size()) +
                             " points, expected at least 2");
  }
  if (points.size() > kMaxDngToneCurvePoints) {
    throw std::runtime_error("ProfileToneCurve has " + std::to_string(points.size()) +
                             " points, above the limit of " +
                             std::to_string(kMaxDngToneCurvePoints));
  }
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto [x, y] = points[i];
    if (!std::isfinite(x) || !std::isfinite(y)) FailPoint(i, "a value is not finite");
    if (x < 0.0f || x > 1.0f) FailPoint(i, "x is outside [0, 1]");
    if (y < 0.0f || y > 1.0f) FailPoint(i, "y is outside [0, 1]");
    if (i > 0 && x <= points[i - 1].x) FailPoint(i, "x does not rise above the previous point");
  }
}

/// Knot slopes of `dng_spline_solver::Solve` (dng_spline.cpp): weighted chord slopes as the first
/// estimate, then the tridiagonal system for C2 continuity with zero second derivative at the
/// ends. The order of operations follows the SDK, so the slopes match it bit for bit.
auto SolveSlopes(const std::vector<DngToneCurvePoint>& points) -> std::vector<double> {
  const auto          count = static_cast<int>(points.size());
  std::vector<double> x(points.size());
  std::vector<double> y(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    x[i] = points[i].x;
    y[i] = points[i].y;
  }
  std::vector<double> s(points.size());
  double              a = x[1] - x[0];
  double              b = (y[1] - y[0]) / a;
  s[0]                  = b;
  for (int j = 2; j < count; ++j) {
    const double c = x[j] - x[j - 1];
    const double d = (y[j] - y[j - 1]) / c;
    s[j - 1]       = (b * c + d * a) / (a + c);
    a              = c;
    b              = d;
  }
  s[count - 1] = 2.0 * b - s[count - 2];
  s[0]         = 2.0 * s[0] - s[1];
  if (count > 2) {
    std::vector<double> e(points.size());
    std::vector<double> f(points.size());
    std::vector<double> g(points.size());
    f[0]         = 0.5;
    e[count - 1] = 0.5;
    g[0]         = 0.75 * (s[0] + s[1]);
    g[count - 1] = 0.75 * (s[count - 2] + s[count - 1]);
    for (int j = 1; j < count - 1; ++j) {
      a    = (x[j + 1] - x[j - 1]) * 2.0;
      e[j] = (x[j + 1] - x[j]) / a;
      f[j] = (x[j] - x[j - 1]) / a;
      g[j] = 1.5 * s[j];
    }
    for (int j = 1; j < count; ++j) {
      a = 1.0 - f[j - 1] * e[j];
      if (j != count - 1) f[j] /= a;
      g[j] = (g[j] - g[j - 1] * e[j]) / a;
    }
    for (int j = count - 2; j >= 0; --j) g[j] = g[j] - f[j] * g[j + 1];
    s = std::move(g);
  }
  return s;
}

/// One cubic segment as a 1-D Bezier (`EvaluateSplineSegment`, dng_spline.h).
auto EvaluateSegment(double x, double x0, double y0, double s0, double x1, double y1, double s1)
    -> double {
  const double a = x1 - x0;
  const double b = (x - x0) / a;
  const double c = (x1 - x) / a;
  return ((y0 * (2.0 - c + b) + (s0 * a * b)) * (c * c)) +
         ((y1 * (2.0 - b + c) - (s1 * a * c)) * (b * b));
}

auto Fingerprint(const std::vector<DngToneCurvePoint>& points) -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto    add  = [&](std::uint32_t word) {
    for (int shift = 0; shift < 32; shift += 8) {
      hash = (hash ^ ((word >> shift) & 0xffu)) * 1099511628211ULL;
    }
  };
  add(static_cast<std::uint32_t>(points.size()));
  for (const auto& point : points) {
    add(std::bit_cast<std::uint32_t>(point.x));
    add(std::bit_cast<std::uint32_t>(point.y));
  }
  return hash;
}

}  // namespace

auto DngToneCurve::Evaluate(double x) const -> double {
  const auto count = static_cast<int>(points.size());
  if (x <= points.front().x) return points.front().y;
  if (x >= points.back().x) return points.back().y;
  // Binary search of dng_spline_solver::Evaluate: points[j - 1].x < x <= points[j].x.
  int lower = 1;
  int upper = count - 1;
  while (upper > lower) {
    const int mid = (lower + upper) >> 1;
    if (x == points[mid].x) return points[mid].y;
    if (x > points[mid].x) {
      lower = mid + 1;
    } else {
      upper = mid;
    }
  }
  const int j = lower;
  return EvaluateSegment(x, points[j - 1].x, points[j - 1].y, slopes[j - 1], points[j].x,
                         points[j].y, slopes[j]);
}

auto DngToneCurve::Interpolate(float x) const -> float {
  // dng_1d_table::Interpolate in float arithmetic. Its guard entry after the last sample repeats
  // that sample, so x = 1 returns table[4096].
  const float y     = std::clamp(x, 0.0f, 1.0f) * static_cast<float>(kDngToneTableIntervals);
  const auto  index = static_cast<std::size_t>(y);
  if (index >= kDngToneTableIntervals) return table[kDngToneTableIntervals];
  const float fract = y - static_cast<float>(index);
  return table[index] * (1.0f - fract) + table[index + 1] * fract;
}

auto MakeDngToneCurve(std::vector<DngToneCurvePoint> points) -> DngToneCurvePtr {
  ValidatePoints(points);
  auto curve        = std::make_shared<DngToneCurve>();
  curve->slopes     = SolveSlopes(points);
  curve->points     = std::move(points);
  // dng_1d_table::Initialize without subsampling: the function at j * (1 / 4096).
  const double step = 1.0 / static_cast<double>(kDngToneTableIntervals);
  for (std::size_t j = 0; j < kDngToneTableEntries; ++j) {
    curve->table[j] = static_cast<float>(curve->Evaluate(static_cast<double>(j) * step));
  }
  curve->fingerprint = Fingerprint(curve->points);
  return curve;
}

void ApplyDngBaselineRgbTone(const DngToneCurve& curve, std::span<float, 3> rgb) {
  const float r    = rgb[0];
  const float g    = rgb[1];
  const float b    = rgb[2];
  // RefBaselineRGBTone (dng_reference.cpp): tone the largest (hi) and smallest (lo) channel, and
  // place the middle channel at its relative position between them.
  const auto  tone = [&](float hi, float mid, float lo, float& out_hi, float& out_mid,
                        float& out_lo) {
    out_hi  = curve.Interpolate(hi);
    out_lo  = curve.Interpolate(lo);
    out_mid = out_lo + ((out_hi - out_lo) * (mid - lo) / (hi - lo));
  };
  float rr = 0.0f;
  float gg = 0.0f;
  float bb = 0.0f;
  if (r >= g) {
    if (g > b) {
      tone(r, g, b, rr, gg, bb);  // r >= g > b
    } else if (b > r) {
      tone(b, r, g, bb, rr, gg);  // b > r >= g
    } else if (b > g) {
      tone(r, b, g, rr, bb, gg);  // r >= b > g
    } else {
      rr = curve.Interpolate(r);  // r >= g == b
      gg = curve.Interpolate(g);
      bb = gg;
    }
  } else {
    if (r >= b) {
      tone(g, r, b, gg, rr, bb);  // g > r >= b
    } else if (b > g) {
      tone(b, g, r, bb, gg, rr);  // b > g > r
    } else {
      tone(g, b, r, gg, bb, rr);  // g >= b > r
    }
  }
  rgb[0] = rr;
  rgb[1] = gg;
  rgb[2] = bb;
}

}  // namespace alcedo
