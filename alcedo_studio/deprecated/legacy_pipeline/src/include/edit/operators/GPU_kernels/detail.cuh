//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of detail enhancement operators

#pragma once

#include <cuda_runtime.h>

#include "edit/operators/op_kernel.hpp"
#include "edit/runtime/cuda/cuda_detail_math.cuh"
#include "param.cuh"

namespace alcedo {
namespace CUDA {

// Optimized clarity vertical-pass kernel using dynamic shared memory.
// Instead of uncoalesced global loads for the vertical blur, the block
// collaboratively caches the required tile in shared memory.
__global__ void ClarityVerticalApplyKernel(const float4* __restrict src, float4* __restrict dst,
                                           int width, int height, size_t pitch_elems,
                                           GPUOperatorParams params) {
  extern __shared__ float4 smem[];

  const int tap_count = params.clarity_gaussian_tap_count_;
  const int radius    = tap_count - 1;

  const int tx = threadIdx.x;
  const int ty = threadIdx.y;
  const int x  = blockIdx.x * blockDim.x + tx;
  const int y  = blockIdx.y * blockDim.y + ty;

  // Early-out / copy-through for disabled state
  if (radius < 0 || !params.clarity_enabled_ || params.clarity_offset_ == 0.0f) {
    if (x < width && y < height) {
      size_t offset = static_cast<size_t>(y) * pitch_elems + x;
      dst[offset]   = src[offset];
    }
    return;
  }

  const int smem_width  = blockDim.x;
  const int smem_height = blockDim.y + 2 * radius;

  // Collaborative load of the source tile into shared memory.
  const int thread_id   = ty * blockDim.x + tx;
  const int block_size  = blockDim.x * blockDim.y;
  const int total_loads = smem_height * smem_width;

  for (int idx = thread_id; idx < total_loads; idx += block_size) {
    int ly = idx / smem_width;
    int lx = idx % smem_width;

    int src_x = blockIdx.x * blockDim.x + lx;
    int src_y = blockIdx.y * blockDim.y + ly - radius;
    src_y     = max(0, min(src_y, height - 1));

    if (src_x < width) {
      smem[ly * smem_width + lx] = src[static_cast<size_t>(src_y) * pitch_elems + src_x];
    }
  }
  __syncthreads();

  if (x >= width || y >= height) return;

  // Vertical Gaussian blur from shared memory
  const int cy = ty + radius;
  const int cx = tx;

  const float* weights = params.clarity_gaussian_weights_;
  float4       blur    = smem[cy * smem_width + cx];
  blur.x *= weights[0];
  blur.y *= weights[0];
  blur.z *= weights[0];
  blur.w *= weights[0];

#pragma unroll 4
  for (int tap = 1; tap < tap_count; ++tap) {
    float  w = weights[tap];
    float4 a = smem[(cy + tap) * smem_width + cx];
    float4 b = smem[(cy - tap) * smem_width + cx];
    blur.x += (a.x + b.x) * w;
    blur.y += (a.y + b.y) * w;
    blur.z += (a.z + b.z) * w;
    blur.w += (a.w + b.w) * w;
  }

  // Read original pixel
  const size_t offset = static_cast<size_t>(y) * pitch_elems + x;
  const float4 orig   = dst[offset];

  // Local contrast enhancement with edge protection
  float4 diff = make_float4(orig.x - blur.x, orig.y - blur.y, orig.z - blur.z, 0.0f);

  float diff_lum = detail_luminance(diff);
  float edge_mag = fabsf(diff_lum);
  // Edge threshold tuned for medium-radius clarity (approx 15-20px sigma)
  constexpr float kEdgeThreshold = 0.18f;
  float protect = 1.0f - detail_smoothstep(0.0f, kEdgeThreshold, edge_mag);

  float lum   = detail_luminance(orig);
  float t_lum = (lum - 0.5f) * 2.0f;
  float mask  = 1.0f - t_lum * t_lum;
  mask        = fmaxf(mask, 0.0f);

  float strength = params.clarity_offset_ * protect * mask;

  dst[offset] = make_float4(fmaf(diff.x, strength, orig.x), fmaf(diff.y, strength, orig.y),
                            fmaf(diff.z, strength, orig.z), orig.w);
}

struct GPU_ClarityBlurHorizontalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems,
                                             GPUOperatorParams& params) const {
    const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    if (!params.clarity_enabled_ || params.clarity_offset_ == 0.0f || params.clarity_radius_ <= 0.0f ||
        params.clarity_gaussian_tap_count_ <= 0) {
      dst[offset] = src[offset];
      return;
    }

    dst[offset] = detail_blur_horizontal(x, y, src, width, height, pitch_elems,
                                         params.clarity_gaussian_tap_count_,
                                         params.clarity_gaussian_weights_);
  }
};

struct GPU_ClarityApplyVerticalKernel : GPUNeighborOpTag {
  // Fallback device functor (used when custom dispatch is unavailable)
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems,
                                             GPUOperatorParams& params) const {
    const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    if (!params.clarity_enabled_ || params.clarity_offset_ == 0.0f || params.clarity_radius_ <= 0.0f ||
        params.clarity_gaussian_tap_count_ <= 0) {
      dst[offset] = src[offset];
      return;
    }

    const float4 orig = dst[offset];
    const float4 blur = detail_blur_vertical(x, y, src, width, height, pitch_elems,
                                             params.clarity_gaussian_tap_count_,
                                             params.clarity_gaussian_weights_);

    float4 diff = make_float4(orig.x - blur.x, orig.y - blur.y, orig.z - blur.z, 0.0f);

    float diff_lum = detail_luminance(diff);
    float edge_mag = fabsf(diff_lum);
    constexpr float kEdgeThreshold = 0.18f;
    float protect = 1.0f - detail_smoothstep(0.0f, kEdgeThreshold, edge_mag);

    float lum   = detail_luminance(orig);
    float t_lum = (lum - 0.5f) * 2.0f;
    float mask  = 1.0f - t_lum * t_lum;
    mask        = fmaxf(mask, 0.0f);

    float strength = params.clarity_offset_ * protect * mask;

    dst[offset] = make_float4(fmaf(diff.x, strength, orig.x), fmaf(diff.y, strength, orig.y),
                              fmaf(diff.z, strength, orig.z), orig.w);
  }

  // Host-side custom dispatch with shared-memory optimisation.
  void Dispatch(float4* src, float4* dst, int width, int height, size_t pitch_elems,
                GPUOperatorParams& params, dim3 grid, dim3 block, cudaStream_t stream) const {
    int radius = params.clarity_gaussian_tap_count_ - 1;
    if (radius < 0) radius = 0;
    size_t shared_mem = (block.y + 2 * radius) * block.x * sizeof(float4);
    ClarityVerticalApplyKernel<<<grid, block, shared_mem, stream>>>(src, dst, width, height,
                                                                    pitch_elems, params);
  }
};

struct GPU_SharpenBlurHorizontalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems,
                                             GPUOperatorParams& params) const {
    const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    if (!params.sharpen_enabled_ || params.sharpen_offset_ == 0.0f || params.sharpen_radius_ <= 0.0f ||
        params.sharpen_gaussian_tap_count_ <= 0) {
      dst[offset] = src[offset];
      return;
    }

    dst[offset] = detail_blur_horizontal(x, y, src, width, height, pitch_elems,
                                         params.sharpen_gaussian_tap_count_,
                                         params.sharpen_gaussian_weights_);
  }
};

struct GPU_SharpenApplyVerticalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems,
                                             GPUOperatorParams& params) const {
    const size_t offset = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    if (!params.sharpen_enabled_ || params.sharpen_offset_ == 0.0f || params.sharpen_radius_ <= 0.0f ||
        params.sharpen_gaussian_tap_count_ <= 0) {
      dst[offset] = src[offset];
      return;
    }

    const float4 orig = dst[offset];
    const float4 blur = detail_blur_vertical(x, y, src, width, height, pitch_elems,
                                             params.sharpen_gaussian_tap_count_,
                                             params.sharpen_gaussian_weights_);

    float4 high = make_float4(orig.x - blur.x, orig.y - blur.y, orig.z - blur.z,
                              orig.w - blur.w);

    if (params.sharpen_threshold_ > 0.0f) {
      const float hp_gray = detail_luminance(high);
      const float mask    = (fabsf(hp_gray) > params.sharpen_threshold_) ? 1.0f : 0.0f;
      high.x *= mask;
      high.y *= mask;
      high.z *= mask;
      high.w *= mask;
    }

    dst[offset] = make_float4(
        orig.x + high.x * params.sharpen_offset_, orig.y + high.y * params.sharpen_offset_,
        orig.z + high.z * params.sharpen_offset_, orig.w + high.w * params.sharpen_offset_);
  }
};

};  // namespace CUDA
};  // namespace alcedo
