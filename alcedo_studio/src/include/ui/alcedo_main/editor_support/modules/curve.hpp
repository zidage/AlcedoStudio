//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QPointF>
#include <json.hpp>
#include <optional>
#include <vector>

namespace alcedo::ui::curve {

constexpr float kCurveEpsilon          = 1e-6f;
constexpr float kCurveMinPointSpacing  = 1e-3f;
constexpr int   kCurveMaxControlPoints = 12;

struct CurveHermiteCache {
  std::vector<float> h_;
  std::vector<float> m_;
};

auto Clamp01(float v) -> float;
auto DefaultCurveControlPoints() -> std::vector<QPointF>;
auto NormalizeCurveControlPoints(const std::vector<QPointF>& in) -> std::vector<QPointF>;
auto CurveControlPointsEqual(const std::vector<QPointF>& a, const std::vector<QPointF>& b,
                             float eps = 1e-4f) -> bool;
auto BuildCurveHermiteCache(const std::vector<QPointF>& points) -> CurveHermiteCache;
auto EvaluateCurveHermite(float x, const std::vector<QPointF>& points,
                          const CurveHermiteCache& cache) -> float;
auto CurveControlPointsToParams(const std::vector<QPointF>& points) -> nlohmann::json;
auto ParseCurveControlPointsFromParams(const nlohmann::json& params)
    -> std::optional<std::vector<QPointF>>;

}  // namespace alcedo::ui::curve
