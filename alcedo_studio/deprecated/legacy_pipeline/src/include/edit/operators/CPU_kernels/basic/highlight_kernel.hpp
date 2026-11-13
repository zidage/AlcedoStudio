//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {
struct HighlightsOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.highlights_enabled_) return;
    float L    = 0.2126f * p.r_ + 0.7152f * p.g_ + 0.0722f * p.b_;
    float outL = L;
    if (L <= params.highlights_k_) {
      // below knee_start: identity
      outL = L;
    } else if (L < 1.0f) {
      // inside the Hermite segment: parameterize t in [0,1]
      float t   = (L - params.highlights_k_) / params.highlights_dx_;
      // Hermite interpolation:
      float H00 = 2 * t * t * t - 3 * t * t + 1;
      float H10 = t * t * t - 2 * t * t + t;
      float H01 = -2 * t * t * t + 3 * t * t;
      float H11 = t * t * t - t * t;
      // note: tangents in Hermite are (dx * m0) and (dx * m1)
      outL      = H00 * params.highlights_k_ + H10 * (params.highlights_dx_ * params.highlights_m0_) +
             H01 * 1.0f + H11 * (params.highlights_dx_ * params.highlights_m1_);
    } else {
      // L >= whitepoint: linear extrapolate using slope m1
      outL = 1.0f + (L - 1.0f) * params.highlights_m1_;
    }

    // avoid negative or NaN
    if (!std::isfinite(outL)) outL = L;
    // Preserve hue/chroma by scaling RGB by ratio outL/L (guard L==0)
    float scale = (L > 1e-8f) ? (outL / L) : 1.0f;
    p.r_ *= scale;
    p.g_ *= scale;
    p.b_ *= scale;
  }
};
}  // namespace alcedo