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
  ApplyUpdate(SharpenUpdate{clamped, std::nullopt, std::nullopt});
}

void SharpenModel::SetRadius(float radius) {
  const float clamped = std::clamp(radius, 0.1f, 64.0f);
  ApplyUpdate(SharpenUpdate{std::nullopt, clamped, std::nullopt});
}

void SharpenModel::SetThreshold(float threshold) {
  const float clamped = std::clamp(threshold, 0.0f, 1.0f);
  ApplyUpdate(SharpenUpdate{std::nullopt, std::nullopt, clamped});
}

void SharpenModel::ApplyUpdate(SharpenUpdate update) {
  if (update.amount.has_value()) {
    update.amount = std::clamp(*update.amount, 0.0f, 100.0f);
  }
  if (update.radius.has_value()) {
    update.radius = std::clamp(*update.radius, 0.1f, 64.0f);
  }
  if (update.threshold.has_value()) {
    update.threshold = std::clamp(*update.threshold, 0.0f, 1.0f);
  }
  MutateWithDirtyFields([update = std::move(update)](SharpenPayload& payload) {
    DirtyFieldMask changed;
    if (update.amount.has_value() && payload.amount != *update.amount) {
      payload.amount = *update.amount;
      changed |= DirtyFieldMask{SharpenDirty::Amount};
    }
    if (update.radius.has_value() && payload.radius != *update.radius) {
      payload.radius = *update.radius;
      changed |= DirtyFieldMask{SharpenDirty::Radius};
    }
    if (update.threshold.has_value() && payload.threshold != *update.threshold) {
      payload.threshold = *update.threshold;
      changed |= DirtyFieldMask{SharpenDirty::Threshold};
    }
    return changed;
  });
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
