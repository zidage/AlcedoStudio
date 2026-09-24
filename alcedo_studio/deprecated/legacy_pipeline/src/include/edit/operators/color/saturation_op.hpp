//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"

namespace alcedo {
class SaturationOp : public OperatorBase<SaturationOp> {
 private:
  /**
   * @brief An relative number for adjusting the saturation from -100 to 100
   *
   */
  float saturation_offset_;

  /**
   * @brief The absolute value for the saturation adjustment from -1.0f to 1.0f
   *
   */
  float scale_;

  void  ComputeScale();

 public:
  static constexpr PriorityLevel     priority_level_    = 6;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Color_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Saturation";
  static constexpr std::string_view  script_name_       = "saturation";
  static constexpr OperatorType      operator_type_     = OperatorType::SATURATION;
  SaturationOp();
  SaturationOp(float saturation_offset);
  SaturationOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo