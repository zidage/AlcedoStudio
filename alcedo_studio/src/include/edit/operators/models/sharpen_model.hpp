//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct SharpenPayload {
  float amount    = 0.0f;
  float radius    = 3.0f;
  float threshold = 0.0f;
};

enum class SharpenDirty : std::uint32_t {
  None      = 0,
  Amount    = 1U << 0,
  Radius    = 1U << 1,
  Threshold = 1U << 2,
  All       = Amount | Radius | Threshold,
};

/**
 * @brief Unsharp-mask parameters. Amount 0 is identity; radius defaults to 3.
 */
class SharpenModel final : public OperatorModelBase<SharpenModel, SharpenPayload, SharpenDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Sharpen(); }
  static constexpr std::string_view kInstanceSuffix = "sharpen";

  [[nodiscard]] auto IsDefault() const -> bool override;

  void SetAmount(float amount);
  void SetRadius(float radius);
  void SetThreshold(float threshold);

  [[nodiscard]] auto Amount() const -> float;
  [[nodiscard]] auto Radius() const -> float;
  [[nodiscard]] auto Threshold() const -> float;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
