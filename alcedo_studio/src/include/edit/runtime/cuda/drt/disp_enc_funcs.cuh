//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Display encoding of the CUDA DRT output. The curves are those of the color encoding catalog
// (color/color_encoding_math.h).

#pragma once

#include <cuda_runtime.h>
#include <vector_functions.h>
#include <vector_types.h>

#include "color/color_encoding_math.h"
#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"
#include "util_funcs.cuh"

namespace alcedo {
namespace CUDA {

GPU_FUNC float3 DisplayEncoding(float3& rgb, float* MAT_limit_to_display, CudaDrtEotf eotf_num,
                                float linear_scale = 1.f) {
  const float3 rgb_disp_linear = mult_f3_f33(rgb, MAT_limit_to_display);
  const float3 scaled          = mult_f_f3(rgb_disp_linear, linear_scale);
  const int    tf              = static_cast<int>(eotf_num);
  const float  hlg_gain = tf == CE_TF_HLG ? CeHlgDisplayGain(scaled.x, scaled.y, scaled.z) : 1.0f;
  return make_float3(CeDisplayEncodeChannel(tf, scaled.x, hlg_gain),
                     CeDisplayEncodeChannel(tf, scaled.y, hlg_gain),
                     CeDisplayEncodeChannel(tf, scaled.z, hlg_gain));
}

}  // namespace CUDA
}  // namespace alcedo
