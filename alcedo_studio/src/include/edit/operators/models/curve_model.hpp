//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <vector>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct CurvePoint {
  float x = 0.0f;
  float y = 0.0f;
};

struct CurvePayload {
  std::vector<CurvePoint> points{{0.0f, 0.0f}, {1.0f, 1.0f}};
};

enum class CurveDirty : std::uint32_t {
  None   = 0,
  Points = 1U << 0,
  All    = Points,
};

/**
 * @brief Tone curve control points in unit square. Identity is (0,0)-(1,1).
 */
class CurveModel final : public OperatorModelBase<CurveModel, CurvePayload, CurveDirty> {
 public:
  static auto TypeId() -> const OperatorTypeId& { return type_ids::Curve(); }
  static constexpr std::string_view kInstanceSuffix = "curve";

  [[nodiscard]] auto IsDefault() const -> bool override;

  void SetPoints(std::vector<CurvePoint> points);
  [[nodiscard]] auto Points() const -> std::vector<CurvePoint>;

  [[nodiscard]] auto ToJson() const -> nlohmann::json override;
  void               LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
