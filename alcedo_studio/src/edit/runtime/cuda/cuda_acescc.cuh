//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

namespace alcedo::cuda_acescc {

__device__ __forceinline__ auto Encode(float value) -> float {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) return kFloor + value;
  if (value < kTransition) return (log2f(kOffset + value * 0.5f) + kA) / kB;
  return (log2f(value) + kA) / kB;
}

__device__ __forceinline__ auto Decode(float value) -> float {
  constexpr float kA         = 9.72f;
  constexpr float kB         = 17.52f;
  constexpr float kOffset    = 0.0000152587890625f;
  constexpr float kFloor     = (-16.0f + kA) / kB;
  constexpr float kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (exp2f(value * kB - kA) - kOffset) * 2.0f;
  return exp2f(value * kB - kA);
}

}  // namespace alcedo::cuda_acescc
