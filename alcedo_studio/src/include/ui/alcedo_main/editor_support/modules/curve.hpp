//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QList>
#include <QPointF>
#include <json.hpp>
#include <optional>
#include <vector>

#include "edit/operators/models/tone_curve_spline.hpp"

namespace alcedo::ui::curve {

constexpr float  kCurveEpsilon          = 1e-6f;
constexpr float  kCurveMinPointSpacing  = 1e-3f;
constexpr int    kCurveMaxControlPoints = static_cast<int>(tone_curve::kMaxControlPoints);

// The curve axis is the ACEScc value of the grade stage: (log2(v) + 9.72) / 17.52.
/// One stop on the curve axis.
constexpr double kCurveAxisStop         = 1.0 / 17.52;
/// 18% grey on the curve axis: (log2(0.18) + 9.72) / 17.52.
constexpr double kCurveAxisGrey         = (-2.4739311883324122 + 9.72) / 17.52;
/// The Fit view of the Tone Curve section: -7 EV to +5 EV from 18% grey.
constexpr double kCurveFitMinimumEv     = -7.0;
constexpr double kCurveFitMaximumEv     = 5.0;
/// Equal steps of x of a sampled curve line.
constexpr int    kCurveLineSteps        = 512;

/// EV from 18% grey of the curve-axis value @p value.
[[nodiscard]] constexpr auto CurveAxisToEv(double value) -> double {
  return (value - kCurveAxisGrey) / kCurveAxisStop;
}
/// Curve-axis value of @p ev stops from 18% grey.
[[nodiscard]] constexpr auto EvToCurveAxis(double ev) -> double {
  return kCurveAxisGrey + ev * kCurveAxisStop;
}

auto Clamp01(float v) -> float;
auto DefaultCurveControlPoints() -> std::vector<QPointF>;
auto NormalizeCurveControlPoints(const std::vector<QPointF>& in) -> std::vector<QPointF>;
auto CurveControlPointsEqual(const std::vector<QPointF>& a, const std::vector<QPointF>& b,
                             float eps = 1e-4f) -> bool;
/// The render spline through normalized control points (see NormalizeCurveControlPoints). The
/// panel evaluates it with tone_curve::EvaluateToneCurveSpline, as the grade kernels do.
auto SolveCurveSpline(const std::vector<QPointF>& points) -> tone_curve::ToneCurveSpline;
/// The line of @p spline on [0, 1]: kCurveLineSteps equal steps of x plus each control point, in
/// increasing x.
auto SampleCurveLine(const tone_curve::ToneCurveSpline& spline) -> QList<QPointF>;
auto CurveControlPointsToParams(const std::vector<QPointF>& points) -> nlohmann::json;
auto ParseCurveControlPointsFromParams(const nlohmann::json& params)
    -> std::optional<std::vector<QPointF>>;

}  // namespace alcedo::ui::curve
