//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Device helpers for the film grain neighborhood pass: reference coordinates, seeded grain samples,
// the 7-tap Gaussian, and the dye clouds of film_grain_math.h. They read only their arguments.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda/prng.hpp"
#include "edit/runtime/film/film_grain_math.h"

namespace alcedo {
namespace CUDA {

__device__ __forceinline__ int FilmGrainReferenceCoord(int coord, int length, int roi_origin,
                                                       float roi_scale, int reference_length,
                                                       bool roi_enabled) {
  const float safe_length = fmaxf(static_cast<float>(length), 1.0f);
  const int   full_extent = max(reference_length > 0 ? reference_length : length, 1);
  const float full_length = static_cast<float>(full_extent);
  const float origin      = roi_enabled ? static_cast<float>(roi_origin) : 0.0f;
  const float span        = roi_enabled ? fmaxf(roi_scale * full_length, 1.0f) : full_length;
  const float mapped = origin + ((static_cast<float>(coord) + 0.5f) * span / safe_length) - 0.5f;
  return min(max(static_cast<int>(floorf(mapped + 0.5f)), 0), full_extent - 1);
}

__device__ __forceinline__ float FilmGrainChannel(float4 value, int channel) {
  if (channel == 0) {
    return value.x;
  }
  if (channel == 1) {
    return value.y;
  }
  return value.z;
}

__device__ __forceinline__ float4 FilmGrainReadClamped(const float4* __restrict src, int x, int y,
                                                       int width, int height, size_t pitch_elems) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x)];
}

__device__ __forceinline__ float FilmGrainClampProbability(float value) {
  return fminf(fmaxf(value, 0.0f), 1.0f);
}

__device__ __forceinline__ float4 FilmGrainApplyDyeClouds(float4 transmittance,
                                                          float4 layer_coverage, float strength) {
  const FilmGrainRgb source{transmittance.x, transmittance.y, transmittance.z};
  const FilmGrainRgb coverage{layer_coverage.x, layer_coverage.y, layer_coverage.z};
  const FilmGrainRgb result = FilmGrainDyeClouds(source, coverage, strength);
  return make_float4(result.r, result.g, result.b, transmittance.w);
}

__device__ __forceinline__ float FilmGrainSample(float probability, int ref_x, int ref_y,
                                                 int channel, std::uint64_t seed) {
  const std::uint64_t stream =
      cuda::PixelStream2D(ref_x, ref_y, static_cast<std::uint32_t>(channel));
  const float draw = cuda::UniformFloat01(seed, stream, 0xd1b54a32d192ed03ULL);
  return draw < FilmGrainClampProbability(probability) ? 1.0f : 0.0f;
}

__device__ __forceinline__ float FilmGrainGaussian7(float c0, float n1, float p1, float n2,
                                                    float p2, float n3, float p3) {
  constexpr float kW0 = 0.49867642f;
  constexpr float kW1 = 0.22831073f;
  constexpr float kW2 = 0.02192964f;
  constexpr float kW3 = 0.00042142f;
  return c0 * kW0 + (n1 + p1) * kW1 + (n2 + p2) * kW2 + (n3 + p3) * kW3;
}

__device__ __forceinline__ float4 FilmGrainBlurVertical(int x, int y, const float4* __restrict src,
                                                        int width, int height, size_t pitch_elems) {
  const float4 c0 = FilmGrainReadClamped(src, x, y, width, height, pitch_elems);
  const float4 n1 = FilmGrainReadClamped(src, x, y - 1, width, height, pitch_elems);
  const float4 p1 = FilmGrainReadClamped(src, x, y + 1, width, height, pitch_elems);
  const float4 n2 = FilmGrainReadClamped(src, x, y - 2, width, height, pitch_elems);
  const float4 p2 = FilmGrainReadClamped(src, x, y + 2, width, height, pitch_elems);
  const float4 n3 = FilmGrainReadClamped(src, x, y - 3, width, height, pitch_elems);
  const float4 p3 = FilmGrainReadClamped(src, x, y + 3, width, height, pitch_elems);

  return make_float4(FilmGrainGaussian7(c0.x, n1.x, p1.x, n2.x, p2.x, n3.x, p3.x),
                     FilmGrainGaussian7(c0.y, n1.y, p1.y, n2.y, p2.y, n3.y, p3.y),
                     FilmGrainGaussian7(c0.z, n1.z, p1.z, n2.z, p2.z, n3.z, p3.z), c0.w);
}

}  // namespace CUDA
}  // namespace alcedo
