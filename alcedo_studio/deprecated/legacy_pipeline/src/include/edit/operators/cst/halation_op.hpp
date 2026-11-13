//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {

class HalationOp : public OperatorBase<HalationOp> {
 public:
  struct HiddenDefaults {
    float low_threshold_  = 0.7f;
    float high_threshold_ = 0.8f;
    float sigma_          = 7.0f;
    float redshift_[3]    = {1.0f, 0.05f, 0.02f};
    float additive_scale_ = 2.0f;
  };

  static constexpr PriorityLevel     priority_level_    = 12;
  static constexpr PipelineStageName affiliation_stage_ = PipelineStageName::Output_Transform;
  static constexpr std::string_view  canonical_name_    = "Halation";
  static constexpr std::string_view  script_name_       = "halation";
  static constexpr OperatorType      operator_type_     = OperatorType::HALATION;

  HalationOp();
  explicit HalationOp(float strength);
  explicit HalationOp(const nlohmann::json& params);

  void               Apply(std::shared_ptr<ImageBuffer> input) override;
  void               ApplyGPU(std::shared_ptr<ImageBuffer> input) override;
  auto               GetParams() const -> nlohmann::json override;
  void               SetParams(const nlohmann::json& params) override;

  void               SetGlobalParams(OperatorParams& params) const override;
  void               EnableGlobalParams(OperatorParams& params, bool enable) override;

  [[nodiscard]] auto strength() const -> float { return strength_; }
  [[nodiscard]] auto strength_scale() const -> float { return strength_scale_; }
  [[nodiscard]] auto hidden_defaults() const -> const HiddenDefaults& { return hidden_defaults_; }

 private:
  void           ComputeScale();

  float          strength_       = 0.0f;
  float          strength_scale_ = 0.0f;
  HiddenDefaults hidden_defaults_;
};

}  // namespace alcedo
