//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/cat02_white_balance_model.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

auto Cat02WhiteBalanceModel::IsDefault() const -> bool {
  return Read([](const Cat02WhiteBalancePayload& payload) {
    return payload.enabled && payload.temperature == kCat02DefaultTemperature &&
           payload.tint == kCat02DefaultTint;
  });
}

void Cat02WhiteBalanceModel::SetEnabled(bool enabled) {
  ApplyUpdate(Cat02WhiteBalanceUpdate{enabled, std::nullopt, std::nullopt});
}

void Cat02WhiteBalanceModel::SetTemperature(float kelvin) {
  ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, kelvin, std::nullopt});
}

void Cat02WhiteBalanceModel::SetTint(float tint) {
  ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, std::nullopt, tint});
}

void Cat02WhiteBalanceModel::ApplyUpdate(Cat02WhiteBalanceUpdate update) {
  if (update.temperature.has_value()) {
    if (!std::isfinite(*update.temperature)) {
      throw std::invalid_argument("CAT02 white balance temperature must be finite");
    }
    update.temperature =
        std::clamp(*update.temperature, kCat02TemperatureMin, kCat02TemperatureMax);
  }
  if (update.tint.has_value()) {
    if (!std::isfinite(*update.tint)) {
      throw std::invalid_argument("CAT02 white balance tint must be finite");
    }
    update.tint = std::clamp(*update.tint, kCat02TintMin, kCat02TintMax);
  }
  MutateWithDirtyFields([update = std::move(update)](Cat02WhiteBalancePayload& payload) {
    DirtyFieldMask changed;
    if (update.enabled.has_value() && payload.enabled != *update.enabled) {
      payload.enabled = *update.enabled;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Enabled};
    }
    if (update.temperature.has_value() && payload.temperature != *update.temperature) {
      payload.temperature = *update.temperature;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Temperature};
    }
    if (update.tint.has_value() && payload.tint != *update.tint) {
      payload.tint = *update.tint;
      changed |= DirtyFieldMask{Cat02WhiteBalanceDirty::Tint};
    }
    return changed;
  });
}

auto Cat02WhiteBalanceModel::Enabled() const -> bool {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.enabled; });
}

auto Cat02WhiteBalanceModel::Temperature() const -> float {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.temperature; });
}

auto Cat02WhiteBalanceModel::Tint() const -> float {
  return Read([](const Cat02WhiteBalancePayload& payload) { return payload.tint; });
}

auto Cat02WhiteBalanceModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  return nlohmann::json{{"enabled", payload.enabled},
                        {"temperature", payload.temperature},
                        {"tint", payload.tint}};
}

void Cat02WhiteBalanceModel::LoadJson(const nlohmann::json& json) {
  ApplyUpdate(Cat02WhiteBalanceUpdate{
      json_util::ReadBool(json, "enabled", true),
      json_util::ReadFloat(json, "temperature", kCat02DefaultTemperature),
      json_util::ReadFloat(json, "tint", kCat02DefaultTint)});
}

}  // namespace alcedo
