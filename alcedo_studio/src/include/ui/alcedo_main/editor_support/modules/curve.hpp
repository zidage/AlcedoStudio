//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <json.hpp>
#include <optional>
#include <vector>

#include "edit/operators/models/tone_curve_spline.hpp"

namespace alcedo::ui::curve {

constexpr float kCurveEpsilon          = 1e-6f;
constexpr float kCurveMinPointSpacing  = 1e-3f;
constexpr int   kCurveMaxControlPoints = static_cast<int>(tone_curve::kMaxControlPoints);

auto Clamp01(float v) -> float;
auto DefaultCurveControlPoints() -> std::vector<QPointF>;
auto NormalizeCurveControlPoints(const std::vector<QPointF>& in) -> std::vector<QPointF>;
auto CurveControlPointsEqual(const std::vector<QPointF>& a, const std::vector<QPointF>& b,
                             float eps = 1e-4f) -> bool;
/// The render spline through normalized control points (see NormalizeCurveControlPoints). The
/// panel evaluates it with tone_curve::EvaluateToneCurveSpline, as the grade kernels do.
auto SolveCurveSpline(const std::vector<QPointF>& points) -> tone_curve::ToneCurveSpline;
auto CurveControlPointsToParams(const std::vector<QPointF>& points) -> nlohmann::json;
auto ParseCurveControlPointsFromParams(const nlohmann::json& params)
    -> std::optional<std::vector<QPointF>>;

}  // namespace alcedo::ui::curve
