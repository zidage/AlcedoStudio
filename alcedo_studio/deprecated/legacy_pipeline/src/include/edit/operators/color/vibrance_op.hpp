//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {
class VibranceOp : public OperatorBase<VibranceOp> {
 private:
  /**
   * @brief An relative number for adjusting the vibrance (natural saturation)
   *
   */
  float vibrance_offset_;

  auto  ComputeScale(float chroma) -> float;

 public:
  static constexpr PriorityLevel     priority_level_    = 7;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Color_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Vibrance";
  static constexpr std::string_view  script_name_       = "vibrance";
  static constexpr OperatorType      operator_type_     = OperatorType::VIBRANCE;
  VibranceOp();
  VibranceOp(float vibrance_offset);
  VibranceOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo