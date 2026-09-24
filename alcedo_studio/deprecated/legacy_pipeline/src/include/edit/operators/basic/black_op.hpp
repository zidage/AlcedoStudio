//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <json.hpp>
#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
class BlackOp : public OperatorBase<BlackOp> {
 private:
  float offset_;

  float y_intercept_;
  float slope_;  // slope of the tone curve

 public:
  auto                               GetScale() -> float;
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "BLACK";
  static constexpr std::string_view  script_name_       = "black";
  static constexpr OperatorType      operator_type_     = OperatorType::BLACK;
  BlackOp()                                             = default;
  BlackOp(float offset);
  BlackOp(const nlohmann::json& params);

  static void GetMask(cv::Mat& src, cv::Mat& mask);

  void        Apply(std::shared_ptr<ImageBuffer> input) override;
  void        ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto        GetParams() const -> nlohmann::json override;
  void        SetParams(const nlohmann::json& params) override;

  void        SetGlobalParams(OperatorParams& params) const override;
  void        EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo