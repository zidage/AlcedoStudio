//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/film_grain_op.hpp"

#include <algorithm>
#include <memory>

namespace alcedo {

FilmGrainOp::FilmGrainOp() { ComputeScale(); }

FilmGrainOp::FilmGrainOp(float strength) : strength_(strength) { ComputeScale(); }

FilmGrainOp::FilmGrainOp(const nlohmann::json& params) { SetParams(params); }

void FilmGrainOp::Apply(std::shared_ptr<ImageBuffer>) {}

void FilmGrainOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {}

auto FilmGrainOp::GetParams() const -> nlohmann::json {
  return {{std::string(script_name_), {{"strength", strength_}}}};
}

void FilmGrainOp::SetParams(const nlohmann::json& params) {
  strength_ = 0.0f;
  if (params.contains(script_name_)) {
    const auto& film_grain = params.at(script_name_);
    if (film_grain.is_number()) {
      strength_ = film_grain.get<float>();
    } else if (film_grain.is_object() && film_grain.contains("strength") &&
               film_grain.at("strength").is_number()) {
      strength_ = film_grain.at("strength").get<float>();
    }
  }
  ComputeScale();
}

void FilmGrainOp::SetGlobalParams(OperatorParams& params) const {
  auto& film_grain         = params.film_grain_;
  film_grain.strength_     = strength_scale_;
  film_grain.filter_sigma_ = hidden_defaults_.filter_sigma_;
  film_grain.seed_         = hidden_defaults_.seed_;
}

void FilmGrainOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  params.film_grain_.enabled_ = enable;
}

void FilmGrainOp::ComputeScale() {
  strength_       = std::clamp(strength_, 0.0f, 100.0f);
  strength_scale_ = (strength_ / 100.0f) / 3.0f;
}

}  // namespace alcedo
