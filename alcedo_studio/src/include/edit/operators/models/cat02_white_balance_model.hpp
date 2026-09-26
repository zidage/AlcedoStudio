//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

/// Identity setting: the ACES AP1 white point (x 0.32168, y 0.33767) under the RAW Custom WB
/// CCT/tint mapping. `AcesWhiteTemperatureTint()` in develop_color_transform.hpp solves the same
/// values; Cat02WhiteBalanceModelTest keeps them equal.
inline constexpr float kCat02DefaultTemperature = 5999.9934f;
inline constexpr float kCat02DefaultTint        = 9.7112783f;
inline constexpr float kCat02TemperatureMin     = 2000.0f;
inline constexpr float kCat02TemperatureMax     = 15000.0f;
inline constexpr float kCat02TintMin            = -150.0f;
inline constexpr float kCat02TintMax            = 150.0f;

struct Cat02WhiteBalancePayload {
  bool  enabled     = true;
  /// Illuminant CCT in Kelvin to adapt to the AP1 white point.
  float temperature = kCat02DefaultTemperature;
  /// Illuminant tint on the RAW Custom WB tint scale.
  float tint        = kCat02DefaultTint;
};

enum class Cat02WhiteBalanceDirty : std::uint32_t {
  None        = 0,
  Enabled     = 1U << 0,
  Temperature = 1U << 1,
  Tint        = 1U << 2,
  All         = Enabled | Temperature | Tint,
};

/**
 * @brief Focused CAT02 white-balance update. Omitted fields retain current values.
 */
struct Cat02WhiteBalanceUpdate {
  std::optional<bool>  enabled;
  std::optional<float> temperature;
  std::optional<float> tint;
};

/**
 * @brief Scene-referred CAT02 white balance on a ColorGrade node.
 *
 * The grade input is treated as balanced to the ACES AP1 white point. Temperature/tint name the
 * illuminant to neutralize, on the same CCT/tint scale as RAW Custom WB; the runtime adapts that
 * chromaticity to the AP1 white with CAT02. The default (AP1 white, about 6000 K) is identity.
 * RAW camera WB lives on DevelopNodeModel.
 */
class Cat02WhiteBalanceModel final
    : public OperatorModelBase<Cat02WhiteBalanceModel, Cat02WhiteBalancePayload,
                               Cat02WhiteBalanceDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Cat02WhiteBalance(); }

  static constexpr std::string_view kInstanceSuffix = "cat02_wb";

  [[nodiscard]] auto                IsDefault() const -> bool override;

  /**
   * @brief Apply validated CAT02 fields atomically and report only changed dirty fields.
   *
   * Temperature and tint are clamped to the RAW Custom WB ranges.
   */
  void                              ApplyUpdate(Cat02WhiteBalanceUpdate update);

  void                              SetEnabled(bool enabled);
  void                              SetTemperature(float kelvin);
  void                              SetTint(float tint);

  [[nodiscard]] auto                Enabled() const -> bool;
  [[nodiscard]] auto                Temperature() const -> float;
  [[nodiscard]] auto                Tint() const -> float;

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
