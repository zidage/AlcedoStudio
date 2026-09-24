//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"

namespace alcedo {

class TintOp : public OperatorBase<TintOp> {
 private:
  /**
   * @brief An relative number for adjusting the tint,
   * negative toward green, positive toward magneta
   * Range from -100 to 100
   */
  float tint_offset_;

  /**
   * @brief An absolute number for adjusting the tint,
   * negative toward green, positive toward magneta
   * Range from -1.0f to 1.0f
   */
  float scale_;

 public:
  static constexpr PriorityLevel     priority_level_    = 4;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Color_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Tint";
  static constexpr std::string_view  script_name_       = "tint";
  static constexpr OperatorType      operator_type_     = OperatorType::TINT;
  TintOp();
  TintOp(float tint_offset);
  TintOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo