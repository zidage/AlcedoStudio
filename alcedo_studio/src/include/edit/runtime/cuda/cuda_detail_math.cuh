//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Device helpers for the unsharp-mask and clarity neighborhood passes: luminance, clamped reads,
// separable Gaussian taps, and smoothstep. They read only their arguments.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>

namespace alcedo {
namespace CUDA {

__device__ __forceinline__ float detail_luminance(const float4& c) {
  // Match the CPU COLOR_BGR2GRAY coefficients.
  return c.x * 0.114f + c.y * 0.587f + c.z * 0.299f;
}

__device__ __forceinline__ float4 detail_read_clamped(const float4* __restrict src, int x, int y,
                                                      int width, int height, size_t pitch_elems) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x)];
}

__device__ __forceinline__ float4 detail_blur_horizontal(int x, int y, const float4* __restrict src,
                                                         int width, int height, size_t pitch_elems,
                                                         int tap_count,
                                                         const float* __restrict weights) {
  if (tap_count <= 0) {
    return detail_read_clamped(src, x, y, width, height, pitch_elems);
  }

  const float4 center = detail_read_clamped(src, x, y, width, height, pitch_elems);
  float4 blur = make_float4(center.x * weights[0], center.y * weights[0], center.z * weights[0],
                            center.w * weights[0]);
  for (int tap = 1; tap < tap_count; ++tap) {
    const float  w = weights[tap];
    const float4 a = detail_read_clamped(src, x + tap, y, width, height, pitch_elems);
    const float4 b = detail_read_clamped(src, x - tap, y, width, height, pitch_elems);
    blur.x += (a.x + b.x) * w;
    blur.y += (a.y + b.y) * w;
    blur.z += (a.z + b.z) * w;
    blur.w += (a.w + b.w) * w;
  }
  return blur;
}

__device__ __forceinline__ float4 detail_blur_vertical(int x, int y, const float4* __restrict src,
                                                       int width, int height, size_t pitch_elems,
                                                       int tap_count,
                                                       const float* __restrict weights) {
  if (tap_count <= 0) {
    return detail_read_clamped(src, x, y, width, height, pitch_elems);
  }

  const float4 center = detail_read_clamped(src, x, y, width, height, pitch_elems);
  float4 blur = make_float4(center.x * weights[0], center.y * weights[0], center.z * weights[0],
                            center.w * weights[0]);
  for (int tap = 1; tap < tap_count; ++tap) {
    const float  w = weights[tap];
    const float4 a = detail_read_clamped(src, x, y + tap, width, height, pitch_elems);
    const float4 b = detail_read_clamped(src, x, y - tap, width, height, pitch_elems);
    blur.x += (a.x + b.x) * w;
    blur.y += (a.y + b.y) * w;
    blur.z += (a.z + b.z) * w;
    blur.w += (a.w + b.w) * w;
  }
  return blur;
}

__device__ __forceinline__ float detail_smoothstep(float edge0, float edge1, float x) {
  float t = fminf(fmaxf((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace CUDA
}  // namespace alcedo
