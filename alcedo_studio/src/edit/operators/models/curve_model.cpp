//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/curve_model.hpp"

#include <utility>

namespace alcedo {

namespace {

auto DirtyBit(CurveChannel channel) -> CurveDirty {
  switch (channel) {
    case CurveChannel::Rgb:
      return CurveDirty::Points;
    case CurveChannel::Red:
      return CurveDirty::Red;
    case CurveChannel::Green:
      return CurveDirty::Green;
    case CurveChannel::Blue:
      return CurveDirty::Blue;
  }
  return CurveDirty::None;
}

/// The member of @p owner (a CurvePayload or a CurveUpdate, const or not) that holds @p channel.
template <class Owner>
auto ChannelMember(Owner& owner, CurveChannel channel) -> auto& {
  switch (channel) {
    case CurveChannel::Red:
      return owner.red;
    case CurveChannel::Green:
      return owner.green;
    case CurveChannel::Blue:
      return owner.blue;
    case CurveChannel::Rgb:
      break;
  }
  return owner.points;
}

auto PointsToJson(const std::vector<CurvePoint>& points) -> nlohmann::json {
  nlohmann::json json = nlohmann::json::array();
  for (const auto& point : points) {
    json.push_back({{"x", point.x}, {"y", point.y}});
  }
  return json;
}

auto PointsFromJson(const nlohmann::json& json, std::string_view key) -> std::vector<CurvePoint> {
  std::vector<CurvePoint> points;
  const std::string       name{key};
  if (json.contains(name) && json[name].is_array()) {
    for (const auto& item : json[name]) {
      CurvePoint point;
      point.x = item.value("x", 0.0f);
      point.y = item.value("y", 0.0f);
      points.push_back(point);
    }
  }
  return points;
}

}  // namespace

auto CurveChannelKey(CurveChannel channel) -> std::string_view {
  switch (channel) {
    case CurveChannel::Rgb:
      return "points";
    case CurveChannel::Red:
      return "red";
    case CurveChannel::Green:
      return "green";
    case CurveChannel::Blue:
      return "blue";
  }
  return "points";
}

auto IdentityCurvePoints() -> std::vector<CurvePoint> { return {{0.0f, 0.0f}, {1.0f, 1.0f}}; }

auto IsIdentityCurve(const std::vector<CurvePoint>& points) -> bool {
  return points.size() == 2 && points[0].x == 0.0f && points[0].y == 0.0f && points[1].x == 1.0f &&
         points[1].y == 1.0f;
}

auto CurveChannelPoints(const CurvePayload& payload, CurveChannel channel)
    -> const std::vector<CurvePoint>& {
  return ChannelMember(payload, channel);
}

auto CurveUpdate::Channel(CurveChannel channel) -> std::optional<std::vector<CurvePoint>>& {
  return ChannelMember(*this, channel);
}

auto CurveUpdate::Channel(CurveChannel channel) const
    -> const std::optional<std::vector<CurvePoint>>& {
  return ChannelMember(*this, channel);
}

auto CurveModel::IsDefault() const -> bool {
  return Read([](const CurvePayload& payload) {
    for (const auto channel : kCurveChannels) {
      if (!IsIdentityCurve(CurveChannelPoints(payload, channel))) {
        return false;
      }
    }
    return true;
  });
}

void CurveModel::SetPoints(std::vector<CurvePoint> points) {
  CurveUpdate update;
  update.points = std::move(points);
  ApplyUpdate(std::move(update));
}

auto CurveModel::Points() const -> std::vector<CurvePoint> {
  return ChannelPoints(CurveChannel::Rgb);
}

void CurveModel::ApplyUpdate(CurveUpdate update) {
  for (const auto channel : kCurveChannels) {
    auto& points = update.Channel(channel);
    if (points.has_value() && points->size() < 2) {
      points = IdentityCurvePoints();
    }
  }
  MutateWithDirtyFields([update = std::move(update)](CurvePayload& payload) mutable {
    DirtyFieldMask dirty{};
    for (const auto channel : kCurveChannels) {
      auto& points = update.Channel(channel);
      if (!points.has_value()) {
        continue;
      }
      auto& current = ChannelMember(payload, channel);
      if (current == *points) {
        continue;
      }
      current = std::move(*points);
      dirty |= DirtyFieldMask{DirtyBit(channel)};
    }
    return dirty;
  });
}

auto CurveModel::ChannelPoints(CurveChannel channel) const -> std::vector<CurvePoint> {
  return Read(
      [channel](const CurvePayload& payload) { return CurveChannelPoints(payload, channel); });
}

auto CurveModel::ToJson() const -> nlohmann::json {
  return Read([](const CurvePayload& payload) {
    nlohmann::json json = {{"points", PointsToJson(payload.points)}};
    for (const auto channel : {CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue}) {
      const auto& points = CurveChannelPoints(payload, channel);
      if (!IsIdentityCurve(points)) {
        json[std::string{CurveChannelKey(channel)}] = PointsToJson(points);
      }
    }
    return json;
  });
}

void CurveModel::LoadJson(const nlohmann::json& json) {
  CurveUpdate update;
  for (const auto channel : kCurveChannels) {
    update.Channel(channel) = PointsFromJson(json, CurveChannelKey(channel));
  }
  ApplyUpdate(std::move(update));
}

}  // namespace alcedo
