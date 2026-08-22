//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/cat02_white_balance_model.hpp"

#include <algorithm>

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

auto Cat02WhiteBalanceModel::IsDefault() const -> bool {
  return Read([](const Cat02WhiteBalancePayload& payload) {
    return payload.enabled && payload.temperature_offset == 0.0f && payload.tint_offset == 0.0f;
  });
}

void Cat02WhiteBalanceModel::SetEnabled(bool enabled) {
  Mutate(Cat02WhiteBalanceDirty::Enabled,
         [enabled](Cat02WhiteBalancePayload& payload) { payload.enabled = enabled; });
}

void Cat02WhiteBalanceModel::SetTemperatureOffset(float offset) {
  const float clamped = std::clamp(offset, -100.0f, 100.0f);
  Mutate(Cat02WhiteBalanceDirty::Temperature, [clamped](Cat02WhiteBalancePayload& payload) {
    payload.temperature_offset = clamped;
  });
}

void Cat02WhiteBalanceModel::SetTintOffset(float offset) {
  const float clamped = std::clamp(offset, -100.0f, 100.0f);
  Mutate(Cat02WhiteBalanceDirty::Tint,
         [clamped](Cat02WhiteBalancePayload& payload) { payload.tint_offset = clamped; });
}

auto Cat02WhiteBalanceModel::Enabled() const -> bool {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.enabled; });
}

auto Cat02WhiteBalanceModel::TemperatureOffset() const -> float {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.temperature_offset; });
}

auto Cat02WhiteBalanceModel::TintOffset() const -> float {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.tint_offset; });
}

auto Cat02WhiteBalanceModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  return nlohmann::json{{"enabled", payload.enabled},
                        {"temperature_offset", payload.temperature_offset},
                        {"tint_offset", payload.tint_offset}};
}

void Cat02WhiteBalanceModel::LoadJson(const nlohmann::json& json) {
  SetEnabled(json_util::ReadBool(json, "enabled", true));
  SetTemperatureOffset(json_util::ReadFloat(json, "temperature_offset", 0.0f));
  SetTintOffset(json_util::ReadFloat(json, "tint_offset", 0.0f));
}

}  // namespace alcedo
