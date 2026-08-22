//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/sharpen_model.hpp"

#include <algorithm>

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

auto SharpenModel::IsDefault() const -> bool {
  return Read([](const SharpenPayload& payload) {
    return payload.amount == 0.0f && payload.radius == 3.0f && payload.threshold == 0.0f;
  });
}

void SharpenModel::SetAmount(float amount) {
  const float clamped = std::clamp(amount, 0.0f, 100.0f);
  Mutate(SharpenDirty::Amount, [clamped](SharpenPayload& payload) { payload.amount = clamped; });
}

void SharpenModel::SetRadius(float radius) {
  const float clamped = std::clamp(radius, 0.1f, 64.0f);
  Mutate(SharpenDirty::Radius, [clamped](SharpenPayload& payload) { payload.radius = clamped; });
}

void SharpenModel::SetThreshold(float threshold) {
  const float clamped = std::clamp(threshold, 0.0f, 1.0f);
  Mutate(SharpenDirty::Threshold,
         [clamped](SharpenPayload& payload) { payload.threshold = clamped; });
}

auto SharpenModel::Amount() const -> float {
  return Read([](const SharpenPayload& payload) { return payload.amount; });
}

auto SharpenModel::Radius() const -> float {
  return Read([](const SharpenPayload& payload) { return payload.radius; });
}

auto SharpenModel::Threshold() const -> float {
  return Read([](const SharpenPayload& payload) { return payload.threshold; });
}

auto SharpenModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  return {{"amount", payload.amount}, {"radius", payload.radius}, {"threshold", payload.threshold}};
}

void SharpenModel::LoadJson(const nlohmann::json& json) {
  SetAmount(json_util::ReadFloat(json, "amount", 0.0f));
  SetRadius(json_util::ReadFloat(json, "radius", 3.0f));
  SetThreshold(json_util::ReadFloat(json, "threshold", 0.0f));
}

}  // namespace alcedo
