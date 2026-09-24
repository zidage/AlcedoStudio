//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <opencv2/core.hpp>

#include "edit/operators/op_base.hpp"
#include "type/type.hpp"

namespace alcedo {

class HLSOp : public OperatorBase<HLSOp> {
 private:
  static constexpr int kHueProfileCount = OperatorParams::kHlsProfileCount;

  cv::Vec3f target_hls_;

  cv::Vec3f hls_adjustment_;

  float     hue_range_;
  float     lightness_range_;
  float     saturation_range_;

  std::array<float, kHueProfileCount>    hue_profile_values_{};
  std::array<cv::Vec3f, kHueProfileCount> hls_adjustment_table_{};
  std::array<float, kHueProfileCount>    hue_range_table_{};
  int                                    active_profile_idx_ = 0;

 public:
  static constexpr PriorityLevel     priority_level_    = 5;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Color_Adjustment;
  static constexpr std::string_view  canonical_name_    = "HLS";
  static constexpr std::string_view  script_name_       = "HLS";
  static constexpr OperatorType      operator_type_     = OperatorType::HLS;
  HLSOp();
  HLSOp(const nlohmann::json& params);

  void SetTargetColor(const cv::Vec3f& bgr_color_normalized);
  void SetAdjustment(const cv::Vec3f& adjustment);
  void SetRanges(float h_range, float l_range, float s_range);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
};  // namespace alcedo
