//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/curve.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace alcedo::ui::curve {

auto Clamp01(float v) -> float { return std::clamp(v, 0.0f, 1.0f); }

auto DefaultCurveControlPoints() -> std::vector<QPointF> {
  return {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};
}

auto NormalizeCurveControlPoints(const std::vector<QPointF>& in) -> std::vector<QPointF> {
  std::vector<QPointF> points;
  points.reserve(in.size());
  for (const auto& p : in) {
    if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
      continue;
    }
    points.emplace_back(Clamp01(static_cast<float>(p.x())), Clamp01(static_cast<float>(p.y())));
  }

  if (points.empty()) {
    return DefaultCurveControlPoints();
  }

  std::sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) {
    if (std::abs(a.x() - b.x()) <= kCurveEpsilon) {
      return a.y() < b.y();
    }
    return a.x() < b.x();
  });

  std::vector<QPointF> deduped;
  deduped.reserve(points.size());
  for (const auto& p : points) {
    if (deduped.empty() || std::abs(p.x() - deduped.back().x()) > kCurveEpsilon) {
      deduped.push_back(p);
    } else {
      deduped.back().setY(p.y());
    }
  }

  deduped.front().setY(Clamp01(static_cast<float>(deduped.front().y())));
  deduped.front().setX(Clamp01(static_cast<float>(deduped.front().x())));
  deduped.back().setX(Clamp01(static_cast<float>(deduped.back().x())));
  deduped.back().setY(Clamp01(static_cast<float>(deduped.back().y())));

  if (deduped.size() < 2 || (deduped.back().x() - deduped.front().x()) <= kCurveMinPointSpacing) {
    return DefaultCurveControlPoints();
  }

  std::vector<QPointF> normalized;
  normalized.reserve(deduped.size());
  normalized.push_back(deduped.front());
  for (size_t i = 1; i + 1 < deduped.size(); ++i) {
    const auto& p = deduped[i];
    if (p.x() <= normalized.back().x() + kCurveMinPointSpacing) {
      continue;
    }
    if (p.x() >= deduped.back().x() - kCurveMinPointSpacing) {
      continue;
    }
    normalized.push_back(p);
    if (static_cast<int>(normalized.size()) >= (kCurveMaxControlPoints - 1)) {
      break;
    }
  }
  normalized.push_back(deduped.back());

  if (normalized.size() < 2 ||
      (normalized.back().x() - normalized.front().x()) <= kCurveMinPointSpacing) {
    return DefaultCurveControlPoints();
  }

  return normalized;
}

auto CurveControlPointsEqual(const std::vector<QPointF>& a, const std::vector<QPointF>& b,
                             float eps) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i].x() - b[i].x()) > eps || std::abs(a[i].y() - b[i].y()) > eps) {
      return false;
    }
  }
  return true;
}

auto SolveCurveSpline(const std::vector<QPointF>& points) -> tone_curve::ToneCurveSpline {
  std::vector<CurvePoint> curve_points;
  curve_points.reserve(points.size());
  for (const auto& point : points) {
    curve_points.push_back(
        CurvePoint{static_cast<float>(point.x()), static_cast<float>(point.y())});
  }
  return tone_curve::SolveToneCurveSpline(curve_points);
}

auto SampleCurveLine(const tone_curve::ToneCurveSpline& spline) -> QList<QPointF> {
  QList<QPointF> line;
  line.reserve(kCurveLineSteps + 1 + static_cast<qsizetype>(spline.count));
  std::uint32_t knot = 0;
  for (int step = 0; step <= kCurveLineSteps; ++step) {
    const double x = static_cast<double>(step) / kCurveLineSteps;
    // The control points between the previous step and this one, so the line passes through them.
    while (knot < spline.count && static_cast<double>(spline.x[knot]) < x) {
      const double knot_x = spline.x[knot];
      if (line.isEmpty() || knot_x > line.back().x()) {
        line.push_back(QPointF(knot_x, spline.y[knot]));
      }
      ++knot;
    }
    line.push_back(QPointF(x, tone_curve::EvaluateToneCurveSpline(spline, static_cast<float>(x))));
  }
  return line;
}

auto CurveControlPointsToParams(const std::vector<QPointF>& points) -> nlohmann::json {
  const auto     normalized = NormalizeCurveControlPoints(points);
  nlohmann::json pts        = nlohmann::json::array();
  for (const auto& p : normalized) {
    pts.push_back({{"x", static_cast<float>(p.x())}, {"y", static_cast<float>(p.y())}});
  }
  return {{"curve", {{"size", normalized.size()}, {"points", std::move(pts)}}}};
}

auto ParseCurveControlPointsFromParams(const nlohmann::json& params)
    -> std::optional<std::vector<QPointF>> {
  if (!params.is_object()) {
    return std::nullopt;
  }

  const nlohmann::json* curve_json = &params;
  if (params.contains("curve")) {
    curve_json = &params["curve"];
  }

  if (!curve_json->is_object()) {
    return std::nullopt;
  }

  std::vector<QPointF> points;

  if (curve_json->contains("points") && (*curve_json)["points"].is_array()) {
    for (const auto& p : (*curve_json)["points"]) {
      if (!p.is_object() || !p.contains("x") || !p.contains("y")) {
        continue;
      }
      try {
        points.emplace_back(p["x"].get<float>(), p["y"].get<float>());
      } catch (...) {
      }
    }
  }

  if (points.empty() && curve_json->contains("size")) {
    size_t point_count = 0;
    try {
      point_count = (*curve_json)["size"].get<size_t>();
    } catch (...) {
      point_count = 0;
    }
    for (size_t i = 0; i < point_count; ++i) {
      const std::string key = std::format("pts{}", i);
      if (!curve_json->contains(key)) {
        continue;
      }
      const auto& p = (*curve_json)[key];
      if (!p.is_object() || !p.contains("x") || !p.contains("y")) {
        continue;
      }
      try {
        points.emplace_back(p["x"].get<float>(), p["y"].get<float>());
      } catch (...) {
      }
    }
  }

  if (points.empty()) {
    return std::nullopt;
  }
  return NormalizeCurveControlPoints(points);
}

}  // namespace alcedo::ui::curve
