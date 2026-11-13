//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/types.hpp>
#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"

namespace alcedo {
struct HighlightCurveParams {
  float       control_;
  float       knee_start_;
  const float slope_range_ = 0.8f;
  float       m1_;

  float       x0_;
  float       x1_ = 1.0f;
  float       y0_;
  float       y1_;

  float       m0_ = 1.0f;

  float       dx_;
};
class HighlightsOp : public OperatorBase<HighlightsOp> {
 private:
  float                offset_;

  HighlightCurveParams curve_;

 public:
  auto                               GetScale() -> float;
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "HIGHLIGHTS";
  static constexpr std::string_view  script_name_       = "highlights";
  static constexpr OperatorType      operator_type_     = OperatorType::HIGHLIGHTS;
  HighlightsOp()                                        = default;
  HighlightsOp(float offset);
  HighlightsOp(const nlohmann::json& params);
  static void GetMask(cv::Mat& src, cv::Mat& mask);
  void        Apply(std::shared_ptr<ImageBuffer> input) override;
  void        ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto        GetParams() const -> nlohmann::json override;
  void        SetParams(const nlohmann::json& params) override;

  void        SetGlobalParams(OperatorParams& params) const override;
  void        EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo