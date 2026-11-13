//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/op_base.hpp"

namespace alcedo {
struct VibranceOpKernel : PointOpTag {
  inline void operator()(Pixel& in, OperatorParams& params) const {
    if (!params.vibrance_enabled_) return;

    float r = in.r_, g = in.g_, b = in.b_;

    float max_val  = std::max({r, g, b});
    float min_val  = std::min({r, g, b});
    float chroma   = max_val - min_val;

    // chroma in [0, max], vibrance_offset in [-100, 100]
    float strength = params.vibrance_offset_ / 100.0f;

    // Protect already highly saturated color
    float falloff  = std::exp(-3.0f * chroma);

    float scale    = 1.0f + strength * falloff;

    if (params.vibrance_offset_ >= 0.0f) {
      float luma = r * 0.299f + g * 0.587f + b * 0.114f;

      r          = luma + (r - luma) * scale;
      g          = luma + (g - luma) * scale;
      b          = luma + (b - luma) * scale;

    } else {
      float avg = (r + g + b) / 3.0f;
      r += (avg - r) * (1.0f - scale);
      g += (avg - g) * (1.0f - scale);
      b += (avg - b) * (1.0f - scale);
    }

    in.r_ = r;
    in.g_ = g;
    in.b_ = b;
  }
};
}  // namespace alcedo