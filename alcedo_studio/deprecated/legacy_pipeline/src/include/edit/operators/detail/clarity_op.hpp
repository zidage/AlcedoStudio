//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
class ClarityOp : public OperatorBase<ClarityOp> {
 private:
  /**
   * @brief Offset to the clarity of the image, ranging from -100 to 100
   *
   */
  float        clarity_offset_;
  /**
   * @brief Scaled offset to the clarity, ranging from -1.0f to 1.0f
   *
   */
  float        scale_;

  /**
   * @brief An internal-use-only parameter to adjust the radius of the USM sharpening filter
   *
   */
  static float usm_radius_;

  void         CreateMidtoneMask(cv::Mat& input, cv::Mat& mask) const;

 public:
  static constexpr PriorityLevel     priority_level_    = 8;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Detail_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Clarity";
  static constexpr std::string_view  script_name_       = "clarity";
  static constexpr OperatorType      operator_type_     = OperatorType::CLARITY;
  ClarityOp();
  ClarityOp(float clarity_offset);
  ClarityOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo