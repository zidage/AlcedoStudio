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

auto BuildCurveHermiteCache(const std::vector<QPointF>& points) -> CurveHermiteCache {
  CurveHermiteCache cache;
  const size_t      n = points.size();
  if (n == 0) {
    return cache;
  }

  cache.m_.assign(n, 0.0f);
  if (n == 1) {
    return cache;
  }

  cache.h_.assign(n - 1, 0.0f);
  std::vector<float> delta(n - 1, 0.0f);
  for (size_t i = 0; i + 1 < n; ++i) {
    const float dx = static_cast<float>(points[i + 1].x() - points[i].x());
    cache.h_[i]    = dx;
    if (std::abs(dx) > kCurveEpsilon) {
      delta[i] = static_cast<float>(points[i + 1].y() - points[i].y()) / dx;
    }
  }

  cache.m_[0]     = delta[0];
  cache.m_[n - 1] = delta[n - 2];
  for (size_t i = 1; i + 1 < n; ++i) {
    if (delta[i - 1] * delta[i] <= 0.0f) {
      cache.m_[i] = 0.0f;
      continue;
    }
    const float w1    = 2.0f * cache.h_[i] + cache.h_[i - 1];
    const float w2    = cache.h_[i] + 2.0f * cache.h_[i - 1];
    const float denom = (w1 / delta[i - 1]) + (w2 / delta[i]);
    cache.m_[i] = ((w1 + w2) > 0.0f && std::abs(denom) > kCurveEpsilon) ? (w1 + w2) / denom : 0.0f;
  }

  return cache;
}

auto EvaluateCurveHermite(float x, const std::vector<QPointF>& points,
                          const CurveHermiteCache& cache) -> float {
  const size_t n = points.size();
  if (n == 0) {
    return Clamp01(x);
  }
  if (n == 1) {
    return Clamp01(static_cast<float>(points.front().y()));
  }
  if (x <= points.front().x()) {
    return Clamp01(static_cast<float>(points.front().y()));
  }
  if (x >= points.back().x()) {
    return Clamp01(static_cast<float>(points.back().y()));
  }

  int idx = static_cast<int>(n) - 2;
  for (int i = 0; i < static_cast<int>(n) - 1; ++i) {
    if (x < points[i + 1].x()) {
      idx = i;
      break;
    }
  }

  if (idx < 0 || static_cast<size_t>(idx) >= cache.h_.size() ||
      static_cast<size_t>(idx + 1) >= cache.m_.size()) {
    return Clamp01(x);
  }

  const float dx = cache.h_[idx];
  if (std::abs(dx) <= kCurveEpsilon) {
    return Clamp01(static_cast<float>(points[idx].y()));
  }

  const float t   = (x - static_cast<float>(points[idx].x())) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;

  const float y   = h00 * static_cast<float>(points[idx].y()) + h10 * dx * cache.m_[idx] +
                  h01 * static_cast<float>(points[idx + 1].y()) + h11 * dx * cache.m_[idx + 1];
  return Clamp01(y);
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
