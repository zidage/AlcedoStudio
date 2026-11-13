//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include <vector>

#include "edit/operators/detail/sharpen_op.hpp"
#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
class SharpenOp : public OperatorBase<SharpenOp> {
 private:
  /**
   * @brief Offset to the sharpness of the image, ranging from 0 to 100
   *
   */
  float offset_    = 0.0f;
  /**
   * @brief Scaled offset to the sharpness of the image, ranging from 0 to 1.0f
   *
   */
  float scale_     = 0.0f;

  /**
   * @brief The USM radius
   *
   */
  float radius_    = 1.0f;
  /**
   * @brief A threshold limiting the sharpening effect, like the "Mask" option in ACR's sharpening
   * module
   *
   */
  float threshold_ = 0.0f;

  void  ComputeScale();

 public:
  static constexpr PriorityLevel     priority_level_    = 8;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Detail_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Sharpen";
  static constexpr std::string_view  script_name_       = "sharpen";
  static constexpr OperatorType      operator_type_     = OperatorType::SHARPEN;
  SharpenOp()                                           = default;
  SharpenOp(float offset, float radius, float threshold);
  SharpenOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo