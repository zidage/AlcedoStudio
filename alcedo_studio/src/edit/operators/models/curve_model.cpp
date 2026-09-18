//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/curve_model.hpp"

#include <algorithm>

namespace alcedo {

namespace {

auto IsIdentityPoints(const std::vector<CurvePoint>& points) -> bool {
  return points.size() == 2 && points[0].x == 0.0f && points[0].y == 0.0f && points[1].x == 1.0f &&
         points[1].y == 1.0f;
}

}  // namespace

auto CurveModel::IsDefault() const -> bool {
  return Read([](const CurvePayload& payload) { return IsIdentityPoints(payload.points); });
}

void CurveModel::SetPoints(std::vector<CurvePoint> points) {
  if (points.size() < 2) {
    points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
  }
  MutateWithDirtyFields([points = std::move(points)](CurvePayload& payload) mutable {
    if (payload.points == points) {
      return DirtyFieldMask{};
    }
    payload.points = std::move(points);
    return DirtyFieldMask{CurveDirty::Points};
  });
}

auto CurveModel::Points() const -> std::vector<CurvePoint> {
  return Read([](const CurvePayload& payload) { return payload.points; });
}

auto CurveModel::ToJson() const -> nlohmann::json {
  nlohmann::json points = nlohmann::json::array();
  for (const auto& point : Points()) {
    points.push_back({{"x", point.x}, {"y", point.y}});
  }
  return {{"points", std::move(points)}};
}

void CurveModel::LoadJson(const nlohmann::json& json) {
  std::vector<CurvePoint> points;
  if (json.contains("points") && json["points"].is_array()) {
    for (const auto& item : json["points"]) {
      CurvePoint point;
      point.x = item.value("x", 0.0f);
      point.y = item.value("y", 0.0f);
      points.push_back(point);
    }
  }
  SetPoints(std::move(points));
}

}  // namespace alcedo
