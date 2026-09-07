//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct Cat02WhiteBalancePayload {
  bool  enabled            = true;
  float temperature_offset = 0.0f;
  float tint_offset        = 0.0f;
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
  std::optional<float> temperature_offset;
  std::optional<float> tint_offset;
};

/**
 * @brief Scene-referred CAT02 white-balance offsets on a ColorGrade node.
 *
 * Default offsets are zero (identity). RAW camera WB lives on DevelopNodeModel.
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
   */
  void                              ApplyUpdate(Cat02WhiteBalanceUpdate update);

  void                              SetEnabled(bool enabled);
  void                              SetTemperatureOffset(float offset);
  void                              SetTintOffset(float offset);

  [[nodiscard]] auto                Enabled() const -> bool;
  [[nodiscard]] auto                TemperatureOffset() const -> float;
  [[nodiscard]] auto                TintOffset() const -> float;

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
