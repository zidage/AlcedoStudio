//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"

namespace alcedo {
class WhiteOp : public OperatorBase<WhiteOp> {
 private:
  float offset_;

  float y_intercept_;
  float black_point_;
  float slope_;

 public:
  auto                               GetScale() -> float;
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "WHITE";
  static constexpr std::string_view  script_name_       = "white";
  static constexpr OperatorType      operator_type_     = OperatorType::WHITE;
  WhiteOp()                                             = default;
  WhiteOp(float offset);
  WhiteOp(const nlohmann::json& params);

  static void GetMask(cv::Mat& src, cv::Mat& mask);

  void        Apply(std::shared_ptr<ImageBuffer> input) override;
  void        ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto        GetParams() const -> nlohmann::json override;
  void        SetParams(const nlohmann::json& params) override;

  void        SetGlobalParams(OperatorParams& params) const override;
  void        EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo