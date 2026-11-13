//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/color_wheel_model.hpp"

#include <stdexcept>

#include "edit/operators/models/color_wheel_tone.hpp"
#include "edit/operators/models/json_read.hpp"

namespace alcedo {

namespace {

/// Persisted format version of the tone wheels. JSON without a version is version 1: the ACEScc
/// CDL values that LoadJson converts with color_wheel::ConvertLegacyColorWheel.
constexpr int kColorWheelFormatVersion = 2;

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

auto IsNeutral(const ColorWheelControl& control) -> bool {
  return control.color_offset.x == 0.0f && control.color_offset.y == 0.0f &&
         control.color_offset.z == 0.0f && control.luminance_offset == 0.0f;
}

auto IsIdentity(const ColorWheelPayload& payload) -> bool {
  return IsNeutral(payload.lift) && IsNeutral(payload.gamma) && IsNeutral(payload.gain) &&
         IsNeutral(payload.offset);
}

auto LegacyValuesFromJson(const nlohmann::json& json) -> color_wheel::LegacyColorWheelValues {
  color_wheel::LegacyColorWheelValues legacy;
  if (json.contains("lift")) {
    legacy.lift_ = ControlFromJson(json["lift"], legacy.lift_);
  }
  if (json.contains("gamma")) {
    legacy.gamma_ = ControlFromJson(json["gamma"], legacy.gamma_);
  }
  if (json.contains("gain")) {
    legacy.gain_ = ControlFromJson(json["gain"], legacy.gain_);
  }
  return legacy;
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

auto ColorWheelModel::Offset() const -> ColorWheelControl {
  return Read([](const ColorWheelPayload& payload) { return payload.offset; });
}

void ColorWheelModel::ApplyUpdate(ColorWheelUpdate update) {
  MutateWithDirtyFields([update = std::move(update)](ColorWheelPayload& payload) mutable {
    bool changed = false;
    if (update.version_one_json.has_value()) {
      auto converted =
          color_wheel::ConvertLegacyColorWheel(LegacyValuesFromJson(*update.version_one_json));
      converted.version_one_json = std::move(*update.version_one_json);
      changed = converted.lift != payload.lift || converted.gamma != payload.gamma ||
                converted.gain != payload.gain || converted.offset != payload.offset ||
                converted.version_one_json != payload.version_one_json;
      payload = std::move(converted);
    }
    bool       wheel_changed = false;
    const auto apply_control = [&wheel_changed](ColorWheelControl&             control,
                                                const ColorWheelControlUpdate& change) {
      if (change.disc.has_value() && control.disc != *change.disc) {
        control.disc = *change.disc;
        wheel_changed = true;
      }
      if (change.strength.has_value() && control.strength != *change.strength) {
        control.strength = *change.strength;
        wheel_changed    = true;
      }
      if (change.color_offset.has_value() && control.color_offset != *change.color_offset) {
        control.color_offset = *change.color_offset;
        wheel_changed        = true;
      }
      if (change.luminance_offset.has_value() &&
          control.luminance_offset != *change.luminance_offset) {
        control.luminance_offset = *change.luminance_offset;
        wheel_changed            = true;
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
    if (update.offset.has_value()) {
      apply_control(payload.offset, *update.offset);
    }
    if (wheel_changed) {
      // The values are no longer the converted version 1 values.
      payload.version_one_json.reset();
      changed = true;
    }
    return changed ? DirtyFieldMask{ColorWheelDirty::Wheels} : DirtyFieldMask{};
  });
}

auto ColorWheelModel::ToJson() const -> nlohmann::json {
  const auto payload = PayloadCopy();
  if (payload.version_one_json.has_value()) {
    return *payload.version_one_json;
  }
  return {{"version", kColorWheelFormatVersion},
          {"lift", ControlToJson(payload.lift)},
          {"gamma", ControlToJson(payload.gamma)},
          {"gain", ControlToJson(payload.gain)},
          {"offset", ControlToJson(payload.offset)}};
}

void ColorWheelModel::LoadJson(const nlohmann::json& json) {
  Mutate(ColorWheelDirty::Wheels, [&json](ColorWheelPayload& payload) {
    if (!json.contains("version")) {
      payload                  = color_wheel::ConvertLegacyColorWheel(LegacyValuesFromJson(json));
      payload.version_one_json = json;
      return;
    }
    if (json["version"] != kColorWheelFormatVersion) {
      throw std::invalid_argument("color_wheel: unsupported persisted format version " +
                                  json["version"].dump());
    }
    payload.version_one_json.reset();
    if (json.contains("lift")) {
      payload.lift = ControlFromJson(json["lift"], payload.lift);
    }
    if (json.contains("gamma")) {
      payload.gamma = ControlFromJson(json["gamma"], payload.gamma);
    }
    if (json.contains("gain")) {
      payload.gain = ControlFromJson(json["gain"], payload.gain);
    }
    if (json.contains("offset")) {
      payload.offset = ControlFromJson(json["offset"], payload.offset);
    }
  });
}

}  // namespace alcedo
