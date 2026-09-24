//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/halation_op.hpp"

#include <algorithm>
#include <memory>

namespace alcedo {

HalationOp::HalationOp() { ComputeScale(); }

HalationOp::HalationOp(float strength) : strength_(strength) { ComputeScale(); }

HalationOp::HalationOp(const nlohmann::json& params) { SetParams(params); }

void HalationOp::Apply(std::shared_ptr<ImageBuffer>) {}

void HalationOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {}

auto HalationOp::GetParams() const -> nlohmann::json {
  return {{std::string(script_name_), {{"strength", strength_}}}};
}

void HalationOp::SetParams(const nlohmann::json& params) {
  strength_ = 0.0f;
  if (params.contains(script_name_)) {
    const auto& halation = params.at(script_name_);
    if (halation.is_number()) {
      strength_ = halation.get<float>();
    } else if (halation.is_object() && halation.contains("strength") &&
               halation.at("strength").is_number()) {
      strength_ = halation.at("strength").get<float>();
    }
  }
  ComputeScale();
}

void HalationOp::SetGlobalParams(OperatorParams& params) const {
  auto& halation            = params.halation_;
  halation.strength_        = strength_scale_;
  halation.low_threshold_   = hidden_defaults_.low_threshold_;
  halation.high_threshold_  = hidden_defaults_.high_threshold_;
  halation.sigma_           = hidden_defaults_.sigma_;
  halation.redshift_[0]     = hidden_defaults_.redshift_[0];
  halation.redshift_[1]     = hidden_defaults_.redshift_[1];
  halation.redshift_[2]     = hidden_defaults_.redshift_[2];
  halation.additive_scale_  = hidden_defaults_.additive_scale_;
}

void HalationOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.halation_.enabled_ = enable;
}

void HalationOp::ComputeScale() {
  strength_       = std::clamp(strength_, 0.0f, 100.0f);
  strength_scale_ = (strength_ / 100.0f) * hidden_defaults_.additive_scale_;
}

}  // namespace alcedo
