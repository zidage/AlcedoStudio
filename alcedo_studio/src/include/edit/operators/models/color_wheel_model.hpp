//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct Vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

inline auto operator==(const Vec2f& lhs, const Vec2f& rhs) -> bool {
  return lhs.x == rhs.x && lhs.y == rhs.y;
}

struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline auto operator==(const Vec3f& lhs, const Vec3f& rhs) -> bool {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

struct ColorWheelControl {
  Vec2f disc{};
  float strength = 1.0f;
  Vec3f color_offset{};
  float luminance_offset = 0.0f;
};

inline auto operator==(const ColorWheelControl& lhs, const ColorWheelControl& rhs) -> bool {
  return lhs.disc == rhs.disc && lhs.strength == rhs.strength &&
         lhs.color_offset == rhs.color_offset && lhs.luminance_offset == rhs.luminance_offset;
}

struct ColorWheelPayload {
  ColorWheelControl lift{};
  ColorWheelControl gamma{Vec2f{}, 1.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
  ColorWheelControl gain{Vec2f{}, 1.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
};

enum class ColorWheelDirty : std::uint32_t {
  None   = 0,
  Wheels = 1U << 0,
  All    = Wheels,
};

/**
 * @brief Focused update for one Color Wheel control. Omitted fields retain their values.
 */
struct ColorWheelControlUpdate {
  std::optional<Vec2f> disc;
  std::optional<float> strength;
  std::optional<Vec3f> color_offset;
  std::optional<float> luminance_offset;
};

/**
 * @brief Focused lift/gamma/gain update applied as one Model operation.
 */
struct ColorWheelUpdate {
  std::optional<ColorWheelControlUpdate> lift;
  std::optional<ColorWheelControlUpdate> gamma;
  std::optional<ColorWheelControlUpdate> gain;
};

/**
 * @brief Lift / gamma / gain color wheels. Identity uses zero lift and unity gamma/gain.
 */
class ColorWheelModel final
    : public OperatorModelBase<ColorWheelModel, ColorWheelPayload, ColorWheelDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::ColorWheel(); }
  static constexpr std::string_view kInstanceSuffix = "color_wheel";

  [[nodiscard]] auto                IsDefault() const -> bool override;

  /**
   * @brief Read individual wheel controls through the owning Model lock.
   */
  [[nodiscard]] auto                Lift() const -> ColorWheelControl;
  [[nodiscard]] auto                Gamma() const -> ColorWheelControl;
  [[nodiscard]] auto                Gain() const -> ColorWheelControl;

  /**
   * @brief Apply validated color-wheel fields atomically and avoid dirtying equal values.
   */
  void                              ApplyUpdate(ColorWheelUpdate update);

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
