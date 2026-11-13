//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <hwy/highway.h>

#include <string>
#include <string_view>

#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"
#include "type/type.hpp"

namespace alcedo {
class ExposureOp : public OperatorBase<ExposureOp>, PointOpTag {
 private:
  /**
   * @brief An EV offset applied to the target image.
   * Positive to increase the brightness, negative to darken.
   *
   */
  float exposure_offset_;

  /**
   * @brief The actual luminance offset derived from the EV
   * dL = 2^E
   *
   */
  float scale_;

  float gamma_;

 public:
  static constexpr PriorityLevel     priority_level_    = 0;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Exposure";
  static constexpr std::string_view  script_name_       = "exposure";
  static constexpr OperatorType      operator_type_     = OperatorType::EXPOSURE;
  ExposureOp();
  ExposureOp(float exposure_offset);
  ExposureOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo