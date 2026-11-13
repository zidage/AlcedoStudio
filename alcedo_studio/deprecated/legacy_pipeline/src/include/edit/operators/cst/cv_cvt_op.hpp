//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>

#include "edit/operators/op_base.hpp"

namespace alcedo {

/**
 * @brief Already deprecated. Use CSTOp instead.
 *
 */
class CVCvtColorOp : public OperatorBase<CVCvtColorOp> {
 private:
  int                   code_;
  std::optional<size_t> channel_index_;

 public:
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Basic_Adjustment;
  static constexpr std::string_view  canonical_name_    = "CV CvtColor";
  static constexpr std::string_view  script_name_       = "CV_CvtColor";

  CVCvtColorOp()                                        = default;
  CVCvtColorOp(int code, std::optional<size_t> channel_index);
  CVCvtColorOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
}  // namespace alcedo