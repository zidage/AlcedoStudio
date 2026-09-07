//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/color_wheel_model.hpp"

#include "edit/operators/models/json_read.hpp"

namespace alcedo {

namespace {

auto ControlToJson(const ColorWheelControl& control) -> nlohmann::json {
  return {{"disc", {{"x", control.disc.x}, {"y", control.disc.y}}},
          {"strength", control.strength},
          {"color_offset",
           {{"x", control.color_offset.x},
            {"y", control.color_offset.y},
            {"z", control.color_offset.z}}},
          {"luminance_offset", control.luminance_offset}};
}

auto ControlFromJson(const nlohmann::json& json, ColorWheelControl fallback) -> ColorWheelControl {
  if (!json.is_object()) {
    return fallback;
  }
  ColorWheelControl control = fallback;
  if (json.contains("disc") && json["disc"].is_object()) {
    control.disc.x = json_util::ReadFloat(json["disc"], "x", control.disc.x);
    control.disc.y = json_util::ReadFloat(json["disc"], "y", control.disc.y);
  }
  control.strength = json_util::ReadFloat(json, "strength", control.strength);
  if (json.contains("color_offset") && json["color_offset"].is_object()) {
    control.color_offset.x =
        json_util::ReadFloat(json["color_offset"], "x", control.color_offset.x);
    control.color_offset.y =
        json_util::ReadFloat(json["color_offset"], "y", control.color_offset.y);
    control.color_offset.z =
        json_util::ReadFloat(json["color_offset"], "z", control.color_offset.z);
  }
  control.luminance_offset =
      json_util::ReadFloat(json, "luminance_offset", control.luminance_offset);
  return control;
}

auto IsIdentity(const ColorWheelPayload& payload) -> bool {
  const auto lift_ok = payload.lift.disc.x == 0.0f && payload.lift.disc.y == 0.0f &&
                       payload.lift.color_offset.x == 0.0f && payload.lift.color_offset.y == 0.0f &&
                       payload.lift.color_offset.z == 0.0f && payload.lift.luminance_offset == 0.0f;
  const auto gamma_ok = payload.gamma.color_offset.x == 1.0f &&
                        payload.gamma.color_offset.y == 1.0f &&
                        payload.gamma.color_offset.z == 1.0f;
  const auto gain_ok = payload.gain.color_offset.x == 1.0f && payload.gain.color_offset.y == 1.0f &&
                       payload.gain.color_offset.z == 1.0f;
  return lift_ok && gamma_ok && gain_ok;
}

}  // namespace

auto ColorWheelModel::IsDefault() const -> bool {
  return Read([](const ColorWheelPayload& payload) { return IsIdentity(payload); });
}

auto ColorWheelModel::Lift() const -> ColorWheelControl {
  return Read([](const ColorWheelPayload& payload) { return payload.lift; });
}

auto ColorWheelModel::Gamma() const -> ColorWheelControl {
  return Read([](const ColorWheelPayload& payload) { return payload.gamma; });
}

auto ColorWheelModel::Gain() const -> ColorWheelControl {
  return Read([](const ColorWheelPayload& payload) { return payload.gain; });
}

void ColorWheelModel::ApplyUpdate(ColorWheelUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](ColorWheelPayload& payload) mutable {
    bool       changed       = false;
    const auto apply_control = [&changed](ColorWheelControl&             control,
                                          const ColorWheelControlUpdate& change) {
      if (change.disc.has_value() && control.disc != *change.disc) {
        control.disc = *change.disc;
        changed      = true;
      }
      if (change.strength.has_value() && control.strength != *change.strength) {
        control.strength = *change.strength;
        changed          = true;
      }
      if (change.color_offset.has_value() && control.color_offset != *change.color_offset) {
        control.color_offset = *change.color_offset;
        changed              = true;
      }
      if (change.luminance_offset.has_value() &&
          control.luminance_offset != *change.luminance_offset) {
        control.luminance_offset = *change.luminance_offset;
        changed                  = true;
      }
    };
    if (update.lift.has_value()) {
      apply_control(payload.lift, *update.lift);
    }
    if (update.gamma.has_value()) {
      apply_control(payload.gamma, *update.gamma);
    }
    if (update.gain.has_value()) {
      apply_control(payload.gain, *update.gain);
    }
    return changed ? DirtyFieldMask{ColorWheelDirty::Wheels} : DirtyFieldMask{};
  });
}

auto ColorWheelModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  return {{"lift", ControlToJson(payload.lift)},
          {"gamma", ControlToJson(payload.gamma)},
          {"gain", ControlToJson(payload.gain)}};
}

void ColorWheelModel::LoadJson(const nlohmann::json& json) {
  Mutate(ColorWheelDirty::Wheels, [&json](ColorWheelPayload& payload) {
    if (json.contains("lift")) {
      payload.lift = ControlFromJson(json["lift"], payload.lift);
    }
    if (json.contains("gamma")) {
      payload.gamma = ControlFromJson(json["gamma"], payload.gamma);
    }
    if (json.contains("gain")) {
      payload.gain = ControlFromJson(json["gain"], payload.gain);
    }
  });
}

}  // namespace alcedo
