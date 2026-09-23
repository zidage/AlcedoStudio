//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of helper functions in Lib.Academy.DisplayEncoding.ctl
// Reference:
// https://github.com/aces-aswf/aces-core/blob/main/lib/Lib.Academy.DisplayEncoding.ctl

#pragma once

#include <cuda_runtime.h>
#include <vector_types.h>

#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"

namespace alcedo {
namespace CUDA {
GPU_FUNC float3 mult_f3_f33(const float3& v, const float* m) {
  return make_float3(v.x * m[0] + v.y * m[3] + v.z * m[6], v.x * m[1] + v.y * m[4] + v.z * m[7],
                     v.x * m[2] + v.y * m[5] + v.z * m[8]);
}

GPU_FUNC float3 mult_f_f3(const float3& v, float s) {
  return make_float3(v.x * s, v.y * s, v.z * s);
}
GPU_FUNC float3 clamp_f3(const float3& v, float min_val, float max_val) {
  return make_float3(fminf(fmaxf(v.x, min_val), max_val), fminf(fmaxf(v.y, min_val), max_val),
                     fminf(fmaxf(v.z, min_val), max_val));
}
GPU_FUNC float3 pow_f3(const float3& v, float exp) {
  return make_float3(powf(v.x, exp), powf(v.y, exp), powf(v.z, exp));
}
}  // namespace CUDA
}  // namespace alcedo