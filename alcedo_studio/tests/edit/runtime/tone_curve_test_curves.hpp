//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Tone Curve inputs for the GPU grade tests: control points that cover the ACEScc values of a test
// plane, and the CPU reference of ApplyCurve read from a packed Curve grade slot.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"
#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo::tone_curve_test {

/// Largest absolute difference between a GPU curve value and EvaluateToneCurveSpline. The kernels
/// use the same float operations; the difference is GPU division and fused multiply-add rounding.
inline constexpr float kGpuCurveTolerance = 1.0e-5f;

/// @p count control points over the middle 80% of [@p low, @p high]. Inputs below and above the
/// end points then use the end-slope continuation, and the inputs between them the cubic. The
/// shape lifts the middle and is monotone: y = x + @p lift * w * sin(pi s), with s from 0 at the
/// first point to 1 at the last point and w the width of the points. A negative @p lift lowers the
/// middle; |lift| < 1 / pi keeps the curve monotone. Three points give a curve too.
inline auto LiftedMidtonePoints(std::size_t count, float low, float high, float lift = 0.2f)
    -> std::vector<CurvePoint> {
  constexpr float         kPi   = 3.14159265358979f;
  const float             first = low + 0.1f * (high - low);
  const float             last  = high - 0.1f * (high - low);
  std::vector<CurvePoint> points;
  points.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float s = static_cast<float>(i) / static_cast<float>(count - 1);
    const float x = first + (last - first) * s;
    points.push_back({x, x + lift * (last - first) * std::sin(kPi * s)});
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

/// The solved splines of the four curves of @p model, in CurveChannel order.
inline auto SolveCurveSplines(const CurveModel& model)
    -> std::array<tone_curve::ToneCurveSpline, kCurveChannelCount> {
  std::array<tone_curve::ToneCurveSpline, kCurveChannelCount> splines;
  for (std::size_t index = 0; index < kCurveChannels.size(); ++index) {
    splines[index] = tone_curve::SolveToneCurveSpline(model.ChannelPoints(kCurveChannels[index]));
  }
  return splines;
}

/// Largest absolute difference between @p output and the four curves of @p splines applied to
/// @p input: the RGB curve to each channel, then the curve of the channel.
template <class Pixel>
auto MaxCurveSetError(const std::vector<Pixel>& input, const std::vector<Pixel>& output,
                      const std::array<tone_curve::ToneCurveSpline, kCurveChannelCount>& splines)
    -> float {
  const auto expected = [&splines](float value, std::size_t channel_index) {
    return tone_curve::EvaluateToneCurveSpline(
        splines[channel_index], tone_curve::EvaluateToneCurveSpline(splines[0], value));
  };
  float error = 0.0f;
  for (std::size_t i = 0; i < input.size(); ++i) {
    error = std::max(error, std::fabs(output[i].r - expected(input[i].r, 1)));
    error = std::max(error, std::fabs(output[i].g - expected(input[i].g, 2)));
    error = std::max(error, std::fabs(output[i].b - expected(input[i].b, 3)));
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

/// CPU reference of the Curve branch of the grade kernels. @p records is the first of the four
/// consecutive records of a Curve slot (CurveGradeRuntimeParams); @p pixel has r, g, and b.
template <class Pixel>
void ApplyPackedCurveSlot(Pixel& pixel, const GradeAdjustmentParams* records) {
  pixel.r = ApplyPackedCurve(ApplyPackedCurve(pixel.r, records[0]), records[1]);
  pixel.g = ApplyPackedCurve(ApplyPackedCurve(pixel.g, records[0]), records[2]);
  pixel.b = ApplyPackedCurve(ApplyPackedCurve(pixel.b, records[0]), records[3]);
}

/// Append the records of the Grade slot of @p model to @p records, as the grade pass packs them
/// into its parameter buffer: four for Curve, one otherwise. @return Index of the first record.
inline auto AppendGradeSlotRecords(std::vector<GradeAdjustmentParams>& records,
                                   const IOperatorModel& model, AdjustmentBehavior behavior)
    -> std::size_t {
  const auto start = records.size();
  if (behavior == AdjustmentBehavior::Curve) {
    const auto curves = MakeCurveGradeRuntimeParams(model);
    records.insert(records.end(), curves.begin(), curves.end());
  } else {
    records.push_back(MakeGradeRuntimeParams(model, behavior));
  }
  return start;
}

}  // namespace alcedo::tone_curve_test
