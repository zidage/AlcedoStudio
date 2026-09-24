//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {

struct ShadowsOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.shadows_enabled_) return;
    float L = 0.2126f * p.r_ + 0.7152f * p.g_ + 0.0722f * p.b_;
    if (L <= params.shadows_x0_) {
      p.r_ = 0.0f;
      p.g_ = 0.0f;
      p.b_ = 0.0f;
    } else if (L < params.shadows_x1_) {
      float t    = (L - params.shadows_x0_) / params.shadows_dx_;
      float H00  = 2 * t * t * t - 3 * t * t + 1;
      float H10  = t * t * t - 2 * t * t + t;
      float H01  = -2 * t * t * t + 3 * t * t;
      float H11  = t * t * t - t * t;
      float outL = H00 * params.shadows_y0_ + H10 * (params.shadows_dx_ * params.shadows_m0_) +
                   H01 * params.shadows_y1_ + H11 * (params.shadows_dx_ * params.shadows_m1_);

      if (!std::isfinite(outL)) outL = L;
      float scale = (L > 1e-8f) ? (outL / L) : 1.0f;
      p.r_ *= scale;
      p.g_ *= scale;
      p.b_ *= scale;
    }
  }
};
}  // namespace alcedo