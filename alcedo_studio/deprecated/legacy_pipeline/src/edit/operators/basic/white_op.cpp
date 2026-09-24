//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/basic/white_op.hpp"

#include <string>

#include "edit/operators/op_base.hpp"
#include "edit/operators/utils/functions.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
WhiteOp::WhiteOp(float offset) : offset_(offset) {}

WhiteOp::WhiteOp(const nlohmann::json& params) { SetParams(params); }
auto WhiteOp::GetScale() -> float { return offset_ / 3.0f; }

void WhiteOp::Apply(std::shared_ptr<ImageBuffer> input) { (void)input; }

void WhiteOp::ApplyGPU(std::shared_ptr<ImageBuffer>) {
  throw std::runtime_error("WhiteOp: ApplyGPU not implemented");
}

auto WhiteOp::GetParams() const -> nlohmann::json {
  return {{std::string(script_name_), offset_ * 100.0f}};
}

void WhiteOp::SetParams(const nlohmann::json& params) {
  float value = 0.0f;
  bool  found = false;

  if (params.is_object() && params.contains(script_name_)) {
    value = params[script_name_].get<float>();
    found = true;
  } else if (params.is_array() && params.size() == 2) {
    // Backward compatibility for legacy snapshots serialized as ["white", value].
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
    y_intercept_ = 1.0f;
    black_point_ = 0.0f;
    slope_       = 1.0f;
  } else {
    offset_      = value / 100.0f;
    y_intercept_ = 1.0f + offset_ / 3.0f;
    black_point_ = 0.0f;
    slope_       = (y_intercept_ - black_point_) / 1.0f;
  }
}

void WhiteOp::SetGlobalParams(OperatorParams& params) const {
  params.white_point_ = y_intercept_;
  params.slope_       = (params.white_point_ - params.black_point_);
}

void WhiteOp::EnableGlobalParams(OperatorParams& params, bool enable) {
  if (enable) {
    params.white_point_ = y_intercept_;
    params.slope_       = (params.white_point_ - params.black_point_);
  } else {
    params.white_point_ = 1.0f;
    params.slope_       = 1.0f - params.black_point_;
  }
}
}  // namespace alcedo
