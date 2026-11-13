//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Tone Curve inputs for the GPU grade tests: control points that cover the ACEScc values of a test
// plane, and the CPU reference of ApplyCurve read from a packed Curve grade slot.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"
#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo::tone_curve_test {

/// Largest absolute difference between a GPU curve value and EvaluateToneCurveSpline. The kernels
/// use the same float operations; the difference is GPU division and fused multiply-add rounding.
inline constexpr float kGpuCurveTolerance = 1.0e-5f;

/// @p count control points over the middle 80% of [@p low, @p high]. Inputs below and above the
/// end points then use the end-slope continuation, and the inputs between them the cubic. The
/// shape lifts the middle and is monotone: y = x + 0.2 * w * sin(pi s), with s from 0 at the first
/// point to 1 at the last point and w the width of the points. Three points give a curve too.
inline auto            LiftedMidtonePoints(std::size_t count, float low, float high)
    -> std::vector<CurvePoint> {
  constexpr float         kPi   = 3.14159265358979f;
  const float             first = low + 0.1f * (high - low);
  const float             last  = high - 0.1f * (high - low);
  std::vector<CurvePoint> points;
  points.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float s = static_cast<float>(i) / static_cast<float>(count - 1);
    const float x = first + (last - first) * s;
    points.push_back({x, x + 0.2f * (last - first) * std::sin(kPi * s)});
  }
  return points;
}

struct ChannelValueRange {
  float low  = 0.0f;
  float high = 0.0f;
};

/// Smallest and largest R, G, or B value of @p pixels (any type with r, g, and b members).
template <class Pixel>
auto RgbValueRange(const std::vector<Pixel>& pixels) -> ChannelValueRange {
  ChannelValueRange range{pixels.front().r, pixels.front().r};
  for (const auto& pixel : pixels) {
    for (const float value : {pixel.r, pixel.g, pixel.b}) {
      range.low  = std::min(range.low, value);
      range.high = std::max(range.high, value);
    }
  }
  return range;
}

/// Largest absolute difference between @p output and the spline of each channel of @p input.
template <class Pixel>
auto MaxCurveError(const std::vector<Pixel>& input, const std::vector<Pixel>& output,
                   const tone_curve::ToneCurveSpline& spline) -> float {
  float error = 0.0f;
  for (std::size_t i = 0; i < input.size(); ++i) {
    error = std::max(
        error, std::fabs(output[i].r - tone_curve::EvaluateToneCurveSpline(spline, input[i].r)));
    error = std::max(
        error, std::fabs(output[i].g - tone_curve::EvaluateToneCurveSpline(spline, input[i].g)));
    error = std::max(
        error, std::fabs(output[i].b - tone_curve::EvaluateToneCurveSpline(spline, input[i].b)));
  }
  return error;
}

/// The spline of a packed Curve grade slot (layout of MakeGradeRuntimeParams).
inline auto SplineFromPackedParams(const GradeAdjustmentParams& params)
    -> tone_curve::ToneCurveSpline {
  constexpr std::size_t       kCapacity = tone_curve::kMaxControlPoints;
  tone_curve::ToneCurveSpline spline;
  spline.count = params.count;
  std::copy_n(params.values, kCapacity, spline.x.begin());
  std::copy_n(params.values + kCapacity, kCapacity, spline.y.begin());
  std::copy_n(params.values + 2 * kCapacity, kCapacity, spline.slope.begin());
  return spline;
}

/// CPU reference of the GPU ApplyCurve for one channel value.
inline auto ApplyPackedCurve(float value, const GradeAdjustmentParams& params) -> float {
  return tone_curve::EvaluateToneCurveSpline(SplineFromPackedParams(params), value);
}

}  // namespace alcedo::tone_curve_test
