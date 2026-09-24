//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cmath>

#include "edit/operators/op_base.hpp"
#include "edit/operators/op_kernel.hpp"

namespace alcedo {
struct CurveOpKernel : PointOpTag {
  inline void operator()(Pixel& p, OperatorParams& params) const {
    if (!params.curve_enabled_) return;
    if (params.curve_ctrl_pts_.empty()) return;

    constexpr float kCurveInfluence = 0.65f;

    float lum = 0.2126f * p.r_ + 0.7152f * p.g_ + 0.0722f * p.b_;
    float x   = lum;

    /* Evaluate Curve */
    const size_t curve_count = params.curve_ctrl_pts_.size();
    if (curve_count == 1) {
      x = params.curve_ctrl_pts_.front().y;
    } else if (params.curve_h_.size() < (curve_count - 1) || params.curve_m_.size() < curve_count) {
      // Invalid/incomplete curve cache, treat as identity.
      return;
    } else if (x <= params.curve_ctrl_pts_.front().x) {
      x = params.curve_ctrl_pts_.front().y;
    } else if (x >= params.curve_ctrl_pts_.back().x) {
      x = params.curve_ctrl_pts_.back().y;
    } else {
      // Find segment
      int idx = 0;
      for (int i = 0; i < static_cast<int>(curve_count) - 1; ++i) {
        if (x < params.curve_ctrl_pts_[i + 1].x) {
          idx = i;
          break;
        }
      }

      const float dx = params.curve_h_[idx];
      if (std::abs(dx) <= 1e-8f) {
        x = params.curve_ctrl_pts_[idx].y;
      } else {
        float t   = (x - params.curve_ctrl_pts_[idx].x) / dx;

        // Hermite interpolation
        float h00 = (2 * t * t * t - 3 * t * t + 1);
        float h10 = (t * t * t - 2 * t * t + t);
        float h01 = (-2 * t * t * t + 3 * t * t);
        float h11 = (t * t * t - t * t);

        x = h00 * params.curve_ctrl_pts_[idx].y + h10 * dx * params.curve_m_[idx] +
            h01 * params.curve_ctrl_pts_[idx + 1].y + h11 * dx * params.curve_m_[idx + 1];
      }
    }

    // Blend toward the mapped luminance for finer, less aggressive edits.
    float new_lum = lum + (x - lum) * kCurveInfluence;
    float ratio   = (lum > 1e-5f) ? new_lum / lum : 0.0f;
    p.r_ *= ratio;
    p.g_ *= ratio;
    p.b_ *= ratio;
  }
};
};  // namespace alcedo
