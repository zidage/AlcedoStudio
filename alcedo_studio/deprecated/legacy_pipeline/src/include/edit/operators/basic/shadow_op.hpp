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

struct ShadowCurveParams {
  float       control_;             // [-1, 1]
  float       toe_end_     = 0.25;  // end of toe region in [0,1], e.g. 0.25
  const float slope_range_ = 0.8f;

  // Hermite between x0=0 and x1=toe_end
  float       m0_;         // slope at blackpoint (x0)
  float       m1_ = 1.0f;  // slope at x1 to keep continuity

  float       x0_ = 0.0f;
  float       x1_;         // = toe_end
  float       y0_ = 0.0f;  // identity at x0
  float       y1_;         // = x1, identity at x1

  float       dx_;  // x1 - x0
};
class ShadowsOp : public OperatorBase<ShadowsOp> {
 private:
  float                offset_;
  ShadowCurveParams    curve_{};

  static constexpr int kLutSize = 1024;
  std::vector<float>   lut_;

  float                gamma_;
  float                inv_threshold_ = 1.0f / 0.5f;

  void                 InitializeLUT();

 public:
  auto                               GetScale() -> float;
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Shadows";
  static constexpr std::string_view  script_name_       = "shadows";
  static constexpr OperatorType      operator_type_     = OperatorType::SHADOWS;
  ShadowsOp()                                           = default;
  ShadowsOp(float offset);
  ShadowsOp(const nlohmann::json& params);

  static void GetMask(cv::Mat& src, cv::Mat& mask);

  void        Apply(std::shared_ptr<ImageBuffer> input) override;
  void        ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto        GetParams() const -> nlohmann::json override;
  void        SetParams(const nlohmann::json& params) override;

  void        SetGlobalParams(OperatorParams& params) const override;
  void        EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo