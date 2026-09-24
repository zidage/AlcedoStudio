//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/types.hpp>

#include "edit/operators/op_base.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
class ColorWheelOp : public OperatorBase<ColorWheelOp> {
 public:
  struct WheelControl {
    // Normalized CDL disc control point in [-1, 1], center at (0, 0).
    cv::Point2f disc_{0.0f, 0.0f};
    // Trackball mapping strength scalar.
    float       strength_{0.5f};
    // Derived per-wheel RGB term (offset/slope/power depending on wheel).
    cv::Point3f color_offset_{0.0f, 0.0f, 0.0f};
    // Per-wheel master term, applied uniformly to all channels.
    float       luminance_offset_{0.0f};
  };

 private:
  WheelControl lift_;
  WheelControl gamma_;
  WheelControl gain_;

 public:
  static constexpr PriorityLevel     priority_level_    = 5;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Color_Adjustment;
  static constexpr std::string_view  canonical_name_    = "Color Wheel";
  static constexpr std::string_view  script_name_       = "color_wheel";
  static constexpr OperatorType      operator_type_     = OperatorType::COLOR_WHEEL;
  ColorWheelOp();
  ColorWheelOp(const nlohmann::json& params);

  void Apply(std::shared_ptr<ImageBuffer> input) override;
  void ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto GetParams() const -> nlohmann::json override;
  void SetParams(const nlohmann::json& params) override;

  void SetGlobalParams(OperatorParams& params) const override;
  void EnableGlobalParams(OperatorParams& params, bool enable) override;
};
};  // namespace alcedo

// JSON helpers for cv::Point3f values used in wheel parameter serialization.
namespace cv {
inline void to_json(nlohmann::json& j, const Point3f& p) {
  j = {{"x", p.x}, {"y", p.y}, {"z", p.z}};
}
inline void from_json(const nlohmann::json& j, Point3f& p) {
  j.at("x").get_to(p.x);
  j.at("y").get_to(p.y);
  j.at("z").get_to(p.z);
}
}  // namespace cv
