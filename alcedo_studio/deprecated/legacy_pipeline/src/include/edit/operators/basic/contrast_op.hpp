//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"

namespace alcedo {

class ContrastOp : public OperatorBase<ContrastOp> {
 private:
  /**
   * @brief A relative number for adjusting the image
   *
   */
  float contrast_offset_;
  /**
   * @brief An absolute number to represent the contrast after adjustment
   * Usually, it is computed through dividing 100.0f from the offset
   *
   */
  float scale_;

 public:
  static constexpr PriorityLevel     priority_level_    = 3;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Contrast";
  static constexpr std::string_view  script_name_       = "contrast";
  static constexpr OperatorType      operator_type_     = OperatorType::CONTRAST;
  ContrastOp();
  ContrastOp(float contrast_offset);
  ContrastOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;
  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo