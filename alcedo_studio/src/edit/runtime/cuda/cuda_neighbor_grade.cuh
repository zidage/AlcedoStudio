//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda/prng.hpp"
#include "cuda_acescc.cuh"
#include "edit/operators/GPU_kernels/detail.cuh"
#include "edit/operators/GPU_kernels/film_grain.cuh"
#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo::cuda_neighbor_grade {

__device__ __forceinline__ auto ReadClamped(const float4* src, int x, int y, int width, int height)
    -> float4 {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<std::size_t>(clamped_y) * width + clamped_x];
}

__device__ __forceinline__ auto Scale(float4 value, float factor) -> float4 {
  return make_float4(value.x * factor, value.y * factor, value.z * factor, value.w * factor);
}

__device__ __forceinline__ auto HalationRadius(float sigma) -> int {
  if (!(sigma > 0.0f)) return 0;
  return min(max(static_cast<int>(ceilf(sigma * 3.0f)), 1),
             static_cast<int>(kGradeNeighborMaxTapCount - 1U));
}

__device__ __forceinline__ auto HalationWeight(int tap, float sigma) -> float {
  return tap == 0 ? 1.0f : expf(-static_cast<float>(tap) / fmaxf(sigma, 1.0e-6f));
}

__device__ __forceinline__ auto HalationNormalization(int radius, float sigma) -> float {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) sum += 2.0f * HalationWeight(tap, sigma);
  return 1.0f / fmaxf(sum, 1.0e-6f);
}

__device__ __forceinline__ auto DecodeAcescc(float4 pixel) -> float4 {
  return make_float4(cuda_acescc::Decode(pixel.x), cuda_acescc::Decode(pixel.y),
                     cuda_acescc::Decode(pixel.z), pixel.w);
}

__device__ __forceinline__ auto HalationBlurHorizontal(const float4* src, int x, int y, int width,
                                                       int                        height,
                                                       const GradeNeighborParams& params)
    -> float4 {
  const int   radius = HalationRadius(params.sigma_x);
  const float norm   = HalationNormalization(radius, params.sigma_x);
  const auto  center = DecodeAcescc(ReadClamped(src, x, y, width, height));
  float4      blur   = make_float4(center.x * norm, center.y * norm, center.z * norm, center.w);
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = HalationWeight(tap, params.sigma_x) * norm;
    const auto  left   = DecodeAcescc(ReadClamped(src, x - tap, y, width, height));
    const auto  right  = DecodeAcescc(ReadClamped(src, x + tap, y, width, height));
    blur.x += (left.x + right.x) * weight;
    blur.y += (left.y + right.y) * weight;
    blur.z += (left.z + right.z) * weight;
  }
  return blur;
}

__device__ __forceinline__ auto HalationBlurVertical(const float4* src, int x, int y, int width,
                                                     int height, const GradeNeighborParams& params)
    -> float4 {
  const int   radius = HalationRadius(params.sigma_y);
  const float norm   = HalationNormalization(radius, params.sigma_y);
  float4      blur   = ReadClamped(src, x, y, width, height);
  blur.x *= norm;
  blur.y *= norm;
  blur.z *= norm;
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = HalationWeight(tap, params.sigma_y) * norm;
    const auto  top    = ReadClamped(src, x, y - tap, width, height);
    const auto  bottom = ReadClamped(src, x, y + tap, width, height);
    blur.x += (top.x + bottom.x) * weight;
    blur.y += (top.y + bottom.y) * weight;
    blur.z += (top.z + bottom.z) * weight;
  }
  return blur;
}

__device__ __forceinline__ auto FilmReferenceCoordinate(int x, int y,
                                                        const GradeNeighborParams& params) -> int2 {
  if (params.use_reference_coordinates == 0U) return make_int2(x, y);
  const float mapped_x = params.render_to_reference[0] * (static_cast<float>(x) + 0.5f) +
                         params.render_to_reference[1] * (static_cast<float>(y) + 0.5f) +
                         params.render_to_reference[2] - 0.5f;
  const float mapped_y = params.render_to_reference[3] * (static_cast<float>(x) + 0.5f) +
                         params.render_to_reference[4] * (static_cast<float>(y) + 0.5f) +
                         params.render_to_reference[5] - 0.5f;
  return make_int2(
      min(max(static_cast<int>(floorf(mapped_x + 0.5f)), 0), max(params.reference_width, 1) - 1),
      min(max(static_cast<int>(floorf(mapped_y + 0.5f)), 0), max(params.reference_height, 1) - 1));
}

__device__ __forceinline__ auto FilmSampleAt(const float4* src, int x, int y, int channel,
                                             int width, int height,
                                             const GradeNeighborParams& params) -> float {
  const int  clamped_x = min(max(x, 0), width - 1);
  const int  clamped_y = min(max(y, 0), height - 1);
  const auto signal    = ReadClamped(src, clamped_x, clamped_y, width, height);
  const auto ref       = FilmReferenceCoordinate(clamped_x, clamped_y, params);
  const auto prng_seed = (static_cast<std::uint64_t>(params.seed_hi) << 32U) | params.seed_lo;
  return CUDA::FilmGrainSample(CUDA::FilmGrainChannel(signal, channel), ref.x, ref.y, channel,
                               prng_seed);
}

__device__ __forceinline__ auto FilmBlurHorizontal(const float4* src, int x, int y, int width,
                                                   int height, const GradeNeighborParams& params)
    -> float4 {
  float result[3]{};
  for (int channel = 0; channel < 3; ++channel) {
    float acc = FilmSampleAt(src, x, y, channel, width, height, params) * params.weights[0];
    for (int tap = 1; tap < static_cast<int>(params.tap_count); ++tap) {
      acc += (FilmSampleAt(src, x - tap, y, channel, width, height, params) +
              FilmSampleAt(src, x + tap, y, channel, width, height, params)) *
             params.weights[tap];
    }
    result[channel] = acc;
  }
  return make_float4(result[0], result[1], result[2], ReadClamped(src, x, y, width, height).w);
}

__device__ __forceinline__ auto VerticalRadius(const GradeNeighborParams& params) -> int {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return HalationRadius(params.sigma_y);
  }
  return static_cast<int>(params.radius);
}

__global__ void BlurHorizontal(const float4* src, float4* dst, int width, int height,
                               GradeNeighborParams params) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  float4     result;
  if (behavior == AdjustmentBehavior::Halation) {
    result = HalationBlurHorizontal(src, x, y, width, height, params);
  } else if (behavior == AdjustmentBehavior::FilmGrain) {
    result = FilmBlurHorizontal(src, x, y, width, height, params);
  } else {
    result = CUDA::detail_blur_horizontal(x, y, src, width, height, width,
                                          static_cast<int>(params.tap_count), params.weights);
  }
  dst[static_cast<std::size_t>(y) * width + x] = result;
}

__global__ void ApplyVertical(const float4* original, const float4* blur_horizontal, float4* dst,
                              int width, int height, GradeNeighborParams params) {
  extern __shared__ float4 tile[];
  const int                x            = blockIdx.x * blockDim.x + threadIdx.x;
  const int                y            = blockIdx.y * blockDim.y + threadIdx.y;
  const int                radius       = VerticalRadius(params);
  const int                tile_width   = blockDim.x;
  const int                tile_height  = blockDim.y + 2 * radius;
  const int                thread_id    = threadIdx.y * blockDim.x + threadIdx.x;
  const int                thread_count = blockDim.x * blockDim.y;
  for (int tile_index = thread_id; tile_index < tile_width * tile_height;
       tile_index += thread_count) {
    const int local_x  = tile_index % tile_width;
    const int local_y  = tile_index / tile_width;
    const int source_x = blockIdx.x * blockDim.x + local_x;
    const int source_y = min(max(blockIdx.y * blockDim.y + local_y - radius, 0), height - 1);
    tile[tile_index]   = source_x < width
                             ? blur_horizontal[static_cast<std::size_t>(source_y) * width + source_x]
                             : make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  __syncthreads();
  if (x >= width || y >= height) return;
  const auto index    = static_cast<std::size_t>(y) * width + x;
  const auto source   = original[index];
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  const int  center   = (threadIdx.y + radius) * tile_width + threadIdx.x;

  if (behavior == AdjustmentBehavior::Sharpen) {
    float4 blur = Scale(tile[center], params.weights[0]);
    for (int tap = 1; tap < static_cast<int>(params.tap_count); ++tap) {
      const auto top    = tile[center - tap * tile_width];
      const auto bottom = tile[center + tap * tile_width];
      const auto weight = params.weights[tap];
      blur.x += (top.x + bottom.x) * weight;
      blur.y += (top.y + bottom.y) * weight;
      blur.z += (top.z + bottom.z) * weight;
      blur.w += (top.w + bottom.w) * weight;
    }
    float4 high = make_float4(source.x - blur.x, source.y - blur.y, source.z - blur.z, 0.0f);
    if (params.threshold > 0.0f && fabsf(CUDA::detail_luminance(high)) <= params.threshold) {
      high = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    dst[index] = make_float4(source.x + high.x * params.amount, source.y + high.y * params.amount,
                             source.z + high.z * params.amount, source.w);
    return;
  }

  if (behavior == AdjustmentBehavior::Clarity) {
    float4 blur = Scale(tile[center], params.weights[0]);
    for (int tap = 1; tap < static_cast<int>(params.tap_count); ++tap) {
      const auto top    = tile[center - tap * tile_width];
      const auto bottom = tile[center + tap * tile_width];
      const auto weight = params.weights[tap];
      blur.x += (top.x + bottom.x) * weight;
      blur.y += (top.y + bottom.y) * weight;
      blur.z += (top.z + bottom.z) * weight;
      blur.w += (top.w + bottom.w) * weight;
    }
    const auto  diff = make_float4(source.x - blur.x, source.y - blur.y, source.z - blur.z, 0.0f);
    const float protect =
        1.0f - CUDA::detail_smoothstep(0.0f, 0.18f, fabsf(CUDA::detail_luminance(diff)));
    const float luma     = CUDA::detail_luminance(source);
    const float centered = (luma - 0.5f) * 2.0f;
    const float strength = params.amount * protect * fmaxf(1.0f - centered * centered, 0.0f);
    dst[index] = make_float4(fmaf(diff.x, strength, source.x), fmaf(diff.y, strength, source.y),
                             fmaf(diff.z, strength, source.z), source.w);
    return;
  }

  if (behavior == AdjustmentBehavior::Halation) {
    const float norm = HalationNormalization(radius, params.sigma_y);
    float4      blur = tile[center];
    blur.x *= norm;
    blur.y *= norm;
    blur.z *= norm;
    for (int tap = 1; tap <= radius; ++tap) {
      const float weight = HalationWeight(tap, params.sigma_y) * norm;
      const auto  top    = tile[center - tap * tile_width];
      const auto  bottom = tile[center + tap * tile_width];
      blur.x += (top.x + bottom.x) * weight;
      blur.y += (top.y + bottom.y) * weight;
      blur.z += (top.z + bottom.z) * weight;
    }
    const auto  linear  = DecodeAcescc(source);
    const float spill_r = fmaxf(blur.x - linear.x, 0.0f);
    const float spill_g = fmaxf(blur.y - linear.y, 0.0f);
    const float spill_b = fmaxf(blur.z - linear.z, 0.0f);
    dst[index]          = make_float4(
        cuda_acescc::Encode(linear.x + spill_r * params.amount * params.redshift[0]),
        cuda_acescc::Encode(linear.y + spill_g * params.amount * params.redshift[1]),
        cuda_acescc::Encode(linear.z + spill_b * params.amount * params.redshift[2]), source.w);
    return;
  }

  const auto c0 = tile[center];
  float4     blur = Scale(c0, params.weights[0]);
  for (int tap = 1; tap < static_cast<int>(params.tap_count); ++tap) {
    const auto top    = tile[center - tap * tile_width];
    const auto bottom = tile[center + tap * tile_width];
    const auto weight = params.weights[tap];
    blur.x += (top.x + bottom.x) * weight;
    blur.y += (top.y + bottom.y) * weight;
    blur.z += (top.z + bottom.z) * weight;
  }
  blur.w       = c0.w;
  dst[index]   = CUDA::FilmGrainApplyDyeClouds(source, blur, params.amount);
}

}  // namespace alcedo::cuda_neighbor_grade
