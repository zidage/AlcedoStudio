//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/black_op.hpp"

#include <opencv2/core.hpp>
#include <string>

#include "image/image_buffer.hpp"

namespace alcedo {
BlackOp::BlackOp(float offset) : offset_(offset) {}

BlackOp::BlackOp(const nlohmann::json& params) { SetParams(params); }

auto BlackOp::GetScale() -> float { return offset_ / 3.0f; }

void BlackOp::Apply(std::shared_ptr<ImageBuffer> input) { (void)input; }

void BlackOp::ApplyGPU(std::shared_ptr<ImageBuffer>) { throw std::runtime_error("BlackOp: ApplyGPU not implemented"); }

auto BlackOp::GetParams() const -> nlohmann::json {
  return {{std::string(script_name_), offset_ * 100.0f}};
}

void BlackOp::SetParams(const nlohmann::json& params) {
  float value = 0.0f;
  bool  found = false;

  if (params.is_object() && params.contains(script_name_)) {
    value = params[script_name_].get<float>();
    found = true;
  } else if (params.is_array() && params.size() == 2) {
    // Backward compatibility for legacy snapshots serialized as ["black", value].
    try {
      if (params[0].is_string() && params[0].get<std::string>() == script_name_) {
        value = params[1].get<float>();
        found = true;
      }
    } catch (...) {
    }
  }

  if (!found) {
    offset_      = 0.0f;
    y_intercept_ = 0.0f;
    slope_       = 1.0f;

  } else {
    offset_      = value / 100.0f;
    y_intercept_ = offset_ / 10.f;
    slope_       = (1.0f - y_intercept_) / 1.0f;
  }
}

void BlackOp::SetGlobalParams(OperatorParams& params) const {
  // Should only be called once SetParams has been called
  params.black_point_ = y_intercept_;
  params.slope_       = (params.white_point_ - params.black_point_);
}

void BlackOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  if (enable) {
    params.black_point_ = y_intercept_;
    params.slope_       = (params.white_point_ - params.black_point_);
  } else {
    params.black_point_ = 0.0f;
    params.slope_       = params.white_point_;
  }
}
}  // namespace alcedo
