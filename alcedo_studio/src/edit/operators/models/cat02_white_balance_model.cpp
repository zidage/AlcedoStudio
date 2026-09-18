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
  ApplyUpdate(Cat02WhiteBalanceUpdate{enabled, std::nullopt, std::nullopt});
}

void Cat02WhiteBalanceModel::SetTemperatureOffset(float offset) {
  const float clamped = std::clamp(offset, -100.0f, 100.0f);
  ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, clamped, std::nullopt});
}

void Cat02WhiteBalanceModel::SetTintOffset(float offset) {
  const float clamped = std::clamp(offset, -100.0f, 100.0f);
  ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, std::nullopt, clamped});
}

void Cat02WhiteBalanceModel::ApplyUpdate(Cat02WhiteBalanceUpdate update) {
  if (update.temperature_offset.has_value()) {
    update.temperature_offset = std::clamp(*update.temperature_offset, -100.0f, 100.0f);
  }
  if (update.tint_offset.has_value()) {
    update.tint_offset = std::clamp(*update.tint_offset, -100.0f, 100.0f);
  }
  MutateWithDirtyFields([update = std::move(update)](Cat02WhiteBalancePayload& payload) {
    DirtyFieldMask changed;
    if (update.enabled.has_value() && payload.enabled != *update.enabled) {
      payload.enabled = *update.enabled;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Enabled};
    }
    if (update.temperature_offset.has_value() &&
        payload.temperature_offset != *update.temperature_offset) {
      payload.temperature_offset = *update.temperature_offset;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Temperature};
    }
    if (update.tint_offset.has_value() && payload.tint_offset != *update.tint_offset) {
      payload.tint_offset = *update.tint_offset;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Tint};
    }
    return changed;
  });
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
