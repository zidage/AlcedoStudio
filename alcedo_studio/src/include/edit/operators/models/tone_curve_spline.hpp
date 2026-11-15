//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "edit/operators/models/curve_model.hpp"

// Qt-free spline of the Tone Curve. The Tone Curve panel draws this spline and ApplyCurve of the
// grade kernels (edit/runtime) renders it:
//   - Between the control points: monotone cubic Hermite. The slope at an interior point is the
//     weighted harmonic mean of the two secant slopes (Fritsch and Butland), and 0 when the two
//     secant slopes do not have the same sign. The slope at an end point is the secant slope of
//     the end segment.
//   - Outside the end points: a straight line along the end slope. The identity curve is then an
//     exact identity for all inputs, also below 0 and above 1.
// A segment whose two end slopes equal its secant slope is a straight line, and the evaluation
// gives the same float value as y0 + t * (y1 - y0). A two-point curve thus renders as before.
namespace alcedo::tone_curve {

inline constexpr std::size_t kMinControlPoints = 2;
inline constexpr std::size_t kMaxControlPoints = 16;

/**
 * @brief Solved spline of one curve: the control points and the slope at each point.
 *
 * The GPU layout of a Curve grade slot is x in values[0..15], y in values[16..31], and slope in
 * values[32..47], with count in GradeAdjustmentParams::count.
 */
struct ToneCurveSpline {
  std::uint32_t                        count = 0;
  std::array<float, kMaxControlPoints> x{};
  std::array<float, kMaxControlPoints> y{};
  std::array<float, kMaxControlPoints> slope{};
};

/// The reason why @p points is not a valid spline input, or nullopt when it is valid. Valid
/// points: 2 to 16 points, finite coordinates, and x values that strictly increase.
[[nodiscard]] auto FindToneCurvePointsError(std::span<const CurvePoint> points)
    -> std::optional<std::string>;

/// Solves the slopes of the spline through @p points.
/// @throws std::invalid_argument with the text of FindToneCurvePointsError for invalid points.
[[nodiscard]] auto SolveToneCurveSpline(std::span<const CurvePoint> points) -> ToneCurveSpline;

/// The spline value at @p x. This is the CPU reference of the GPU ApplyCurve.
[[nodiscard]] auto EvaluateToneCurveSpline(const ToneCurveSpline& spline, float x) -> float;

}  // namespace alcedo::tone_curve
