//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/types.hpp>
#include <vector>

#include "edit/operators/op_base.hpp"

namespace alcedo {
class CurveOp : public OperatorBase<CurveOp> {
 private:
  std::vector<cv::Point2f> ctrl_pts_{};
  std::vector<float>       h_{};
  std::vector<float>       m_{};

  void                     ComputeTagents();
  auto                     EvaluateCurve(float x) const -> float;

 public:
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Curve";
  static constexpr std::string_view  script_name_       = "curve";
  static constexpr OperatorType      operator_type_     = OperatorType::CURVE;
  CurveOp()                                             = delete;
  CurveOp(const std::vector<cv::Point2f>& control_points);
  CurveOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  void SetCtrlPts(const std::vector<cv::Point2f>& control_points);
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
};  // namespace alcedo
