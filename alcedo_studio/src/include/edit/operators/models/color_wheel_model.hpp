//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"
#include "json.hpp"

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

namespace color_wheel {

/// Per-channel change in stops of the dominant channel direction at full disc radius.
constexpr float kDiscMaxStops   = 1.0f;
/// Master slider limit in stops.
constexpr float kMasterMaxStops = 2.0f;

}  // namespace color_wheel

/**
 * @brief One color wheel. All values are changes in stops at the target tone of the wheel; the
 * neutral value of every field except strength is 0. See color_wheel_tone.hpp for the tone math.
 */
struct ColorWheelControl {
  /// Disc position in the unit disc.
  Vec2f disc{};
  /// Disc stops at full radius. The color wheel panel writes this value for every edit, so writing
  /// the default panel value back does not change the Model.
  float strength = color_wheel::kDiscMaxStops;
  /// Per-channel disc stops (zero mean). This value, not the disc, sets the rendering.
  Vec3f color_offset{};
  /// Master stops, added to every channel.
  float luminance_offset = 0.0f;
};

inline auto operator==(const ColorWheelControl& lhs, const ColorWheelControl& rhs) -> bool {
  return lhs.disc == rhs.disc && lhs.strength == rhs.strength &&
         lhs.color_offset == rhs.color_offset && lhs.luminance_offset == rhs.luminance_offset;
}

struct ColorWheelPayload {
  ColorWheelControl             lift{};
  ColorWheelControl             gamma{};
  ColorWheelControl             gain{};
  ColorWheelControl             offset{};
  /// Persisted format version 1 JSON that the wheel values were converted from, as stored. ToJson
  /// writes it back until a wheel value changes: the root id of a project and the before / after
  /// values of its history are hashes and byte comparisons of this JSON, so an unedited older
  /// project must serialize to the same bytes.
  std::optional<nlohmann::json> version_one_json;
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
 * @brief Focused lift/gamma/gain/offset update applied as one Model operation.
 */
struct ColorWheelUpdate {
  /// Persisted format version 1 JSON (lift / gamma / gain, without a version key). Replaces every
  /// wheel with its converted values before the control updates apply.
  std::optional<nlohmann::json>          version_one_json;
  std::optional<ColorWheelControlUpdate> lift;
  std::optional<ColorWheelControlUpdate> gamma;
  std::optional<ColorWheelControlUpdate> gain;
  std::optional<ColorWheelControlUpdate> offset;
};

/**
 * @brief Lift / Gamma / Gain / Offset tone color wheels. Identity is zero in every wheel.
 *
 * ToJson writes the persisted format version 2. LoadJson reads JSON without a version as version 1
 * (the ACEScc CDL values of older projects) and converts it to version 2 values.
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
  [[nodiscard]] auto                Offset() const -> ColorWheelControl;

  /**
   * @brief Apply validated color-wheel fields atomically and avoid dirtying equal values.
   */
  void                              ApplyUpdate(ColorWheelUpdate update);

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
