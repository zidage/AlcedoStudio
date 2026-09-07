//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of helper functions in Lib.Academy.Tonescale.ctl
// Reference:
// https://github.com/aces-aswf/aces-core/blob/main/lib/Lib.Academy.Tonescale.ctl

#pragma once

#include <cuda_runtime.h>
#include "edit/operators/GPU_kernels/param.cuh"

namespace alcedo {
namespace CUDA {

GPU_FUNC float Tonescale_fwd(float x, GPU_TSParams& params) {
  // Forward MM tone scale
  // Guard against 0/0 when x == -s_2_ and against negative denominators.
  // +Inf maps to the asymptote (ratio → 1); other non-finite inputs stay at 0.
  if (!isfinite(x)) {
    if (x > 0.0f) {
      const float f_inf = params.m_2_;
      const float h_inf = fmaxf(0.f, f_inf * f_inf / (f_inf + params.t_1_));
      return h_inf * params.n_r_;
    }
    return 0.0f;
  }
  const float denom = x + params.s_2_;
  const float ratio = (denom > 1e-7f) ? (fmaxf(0.f, x) / denom) : 0.0f;
  float f = params.m_2_ * powf(ratio, params.g_);

  float h = fmaxf(0.f, f * f / (f + params.t_1_));
  return h * params.n_r_;
}
}
}