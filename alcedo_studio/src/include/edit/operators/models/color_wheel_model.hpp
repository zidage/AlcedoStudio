//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct Vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct ColorWheelControl {
  Vec2f disc{};
  float strength = 1.0f;
  Vec3f color_offset{};
  float luminance_offset = 0.0f;
};

struct ColorWheelPayload {
  ColorWheelControl lift{};
  ColorWheelControl gamma{Vec2f{}, 1.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
  ColorWheelControl gain{Vec2f{}, 1.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
};

enum class ColorWheelDirty : std::uint32_t {
  None  = 0,
  Wheels = 1U << 0,
  All    = Wheels,
};

/**
 * @brief Lift / gamma / gain color wheels. Identity uses zero lift and unity gamma/gain.
 */
class ColorWheelModel final
    : public OperatorModelBase<ColorWheelModel, ColorWheelPayload, ColorWheelDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::ColorWheel(); }
  static constexpr std::string_view kInstanceSuffix = "color_wheel";

  [[nodiscard]] auto IsDefault() const -> bool override;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
