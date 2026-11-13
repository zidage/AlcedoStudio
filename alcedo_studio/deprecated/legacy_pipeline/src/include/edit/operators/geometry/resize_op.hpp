//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include "edit/operators/geometry/resize_algorithm.hpp"
#include "edit/operators/op_base.hpp"

namespace alcedo {
struct ROI {
  int   x_;
  int   y_;
  float resize_factor_x_;
  float resize_factor_y_;
  float resize_factor_;  // Legacy fallback for old serialized params.
  int   reference_width_ = 0;
  int   reference_height_ = 0;
};

class ResizeOp : public OperatorBase<ResizeOp> {
 private:
  int  maximum_edge_;

  bool enable_scale_ = false;

  bool enable_roi_   = false;
  ROI  roi_          = {0, 0, 1.0f, 1.0f, 1.0f, 0, 0};
  ResizeDownsampleAlgorithm downsample_algorithm_ = ResizeDownsampleAlgorithm::Area;

 public:
  static constexpr PriorityLevel     priority_level_    = 1;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Image_Loading;
  static constexpr std::string_view  canonical_name_    = "Resize";
  static constexpr std::string_view  script_name_       = "resize";
  static constexpr OperatorType      operator_type_     = OperatorType::RESIZE;
  ResizeOp()                                            = default;
  ResizeOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
};  // namespace alcedo
