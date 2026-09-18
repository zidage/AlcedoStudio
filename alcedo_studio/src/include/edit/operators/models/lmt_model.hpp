//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct LmtPayload {
  std::string cube_path;
};

enum class LmtDirty : std::uint32_t {
  None = 0,
  Path = 1U << 0,
  All  = Path,
};

/**
 * @brief Look Modification Transform / LUT path. Empty path is identity.
 */
class LmtModel final : public OperatorModelBase<LmtModel, LmtPayload, LmtDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Lmt(); }
  static constexpr std::string_view kInstanceSuffix = "lmt";

  [[nodiscard]] auto IsDefault() const -> bool override;

  void SetCubePath(std::string path);
  [[nodiscard]] auto CubePath() const -> std::string;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
