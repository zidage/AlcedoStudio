//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include "edit/runtime/cuda/drt/disp_enc_funcs.cuh"
#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/op_kernel.hpp"

namespace alcedo {
namespace CUDA {

GPU_FUNC float4 HalationReadClamped(const float4* __restrict src, int x, int y, int width,
                                    int height, size_t pitch_elems) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x)];
}

GPU_FUNC float3 HalationDecodeDisplayLinear(float4 pixel, const GPUOperatorParams& params) {
  const float3 encoded =
      make_float3(fmaxf(pixel.x, 0.0f), fmaxf(pixel.y, 0.0f), fmaxf(pixel.z, 0.0f));
  return eotf(encoded, params.to_output_params_.eotf);
}

GPU_FUNC float4 HalationEncodeDisplay(float3 linear, float alpha, const GPUOperatorParams& params) {
  const float3 encoded = eotf_inv(linear, params.to_output_params_.eotf);
  return make_float4(encoded.x, encoded.y, encoded.z, alpha);
}

GPU_FUNC int HalationBlurRadius(float sigma) {
  if (!(sigma > 0.0f)) {
    return 0;
  }
  const int radius = static_cast<int>(ceilf(sigma * 3.0f));
  return min(max(radius, 1), OperatorParams::kDetailMaxGaussianTapCount - 1);
}

GPU_FUNC float HalationScaledSigma(float sigma, float render_output_scale) {
  // Halation radius is authored in full-resolution image pixels. Convert it to the
  // current render buffer so resized previews and full-resolution renders keep the
  // same image-space footprint.
  return sigma * fminf(fmaxf(render_output_scale, 1.0e-4f), 1.0f);
}

GPU_FUNC float HalationWeight(int tap, float sigma) {
  return (tap == 0) ? 1.0f : expf(-static_cast<float>(tap) / fmaxf(sigma, 1.0e-6f));
}

GPU_FUNC float HalationWeightNorm(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) {
    sum += 2.0f * HalationWeight(tap, sigma);
  }
  return 1.0f / fmaxf(sum, 1.0e-6f);
}

GPU_FUNC float4 HalationBlurHorizontal(int x, int y, const float4* __restrict src, int width,
                                       int height, size_t pitch_elems,
                                       const GPUOperatorParams& params) {
  const auto&  halation      = params.halation_;
  const float  sigma         = HalationScaledSigma(halation.sigma_, params.render_output_scale_x_);
  const int    radius        = HalationBlurRadius(sigma);
  const float  norm          = HalationWeightNorm(radius, sigma);

  const float4 center        = HalationReadClamped(src, x, y, width, height, pitch_elems);
  const float3 center_linear = HalationDecodeDisplayLinear(center, params);
  float4       blur = make_float4(center_linear.x, center_linear.y, center_linear.z, center.w);
  blur.x *= norm;
  blur.y *= norm;
  blur.z *= norm;

  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight       = HalationWeight(tap, sigma) * norm;
    const float4 left         = HalationReadClamped(src, x - tap, y, width, height, pitch_elems);
    const float4 right        = HalationReadClamped(src, x + tap, y, width, height, pitch_elems);
    const float3 left_linear  = HalationDecodeDisplayLinear(left, params);
    const float3 right_linear = HalationDecodeDisplayLinear(right, params);
    blur.x += (left_linear.x + right_linear.x) * weight;
    blur.y += (left_linear.y + right_linear.y) * weight;
    blur.z += (left_linear.z + right_linear.z) * weight;
  }

  return make_float4(blur.x, blur.y, blur.z,
                     HalationReadClamped(src, x, y, width, height, pitch_elems).w);
}

GPU_FUNC float4 HalationBlurVertical(int x, int y, const float4* __restrict src, int width,
                                     int height, size_t pitch_elems,
                                     const GPUOperatorParams& params) {
  const auto& halation = params.halation_;
  const float sigma    = HalationScaledSigma(halation.sigma_, params.render_output_scale_y_);
  const int   radius   = HalationBlurRadius(sigma);
  const float norm     = HalationWeightNorm(radius, sigma);

  float4      blur     = HalationReadClamped(src, x, y, width, height, pitch_elems);
  blur.x *= norm;
  blur.y *= norm;
  blur.z *= norm;

  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight = HalationWeight(tap, sigma) * norm;
    const float4 top    = HalationReadClamped(src, x, y - tap, width, height, pitch_elems);
    const float4 bottom = HalationReadClamped(src, x, y + tap, width, height, pitch_elems);
    blur.x += (top.x + bottom.x) * weight;
    blur.y += (top.y + bottom.y) * weight;
    blur.z += (top.z + bottom.z) * weight;
  }

  return make_float4(blur.x, blur.y, blur.z,
                     HalationReadClamped(src, x, y, width, height, pitch_elems).w);
}

struct GPU_HalationBlurHorizontalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems, GPUOperatorParams& params) const {
    const size_t offset   = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    const auto&  halation = params.halation_;
    const float  strength = fminf(fmaxf(halation.strength_, 0.0f), 1.0f);

    if (!halation.enabled_ || !(strength > 0.0f)) {
      dst[offset] = src[offset];
      return;
    }

    dst[offset] = HalationBlurHorizontal(x, y, src, width, height, pitch_elems, params);
  }
};

struct GPU_HalationApplyVerticalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems, GPUOperatorParams& params) const {
    const size_t offset   = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    const auto&  halation = params.halation_;
    const float  strength = fminf(fmaxf(halation.strength_, 0.0f), 2.0f);

    if (!halation.enabled_ || !(strength > 0.0f)) {
      dst[offset] = src[offset];
      return;
    }

    const float4 original        = dst[offset];
    const float3 original_linear = HalationDecodeDisplayLinear(original, params);
    const float4 blurred    = HalationBlurVertical(x, y, src, width, height, pitch_elems, params);

    // Inspired by hotgluebanjo/halation-dctl's red-biased blurred-light feedback, but
    // keep only the positive local spill. This preserves flat fields and bright cores while
    // making the halo edge energy respond to strength without widening the blur radius.
    const float  red_gain   = strength * halation.redshift_[0];
    const float  green_gain = strength * halation.redshift_[1];
    const float  blue_gain  = strength * halation.redshift_[2];

    const float3 spill_linear =
        make_float3(fmaxf(blurred.x - original_linear.x, 0.0f),
                    fmaxf(blurred.y - original_linear.y, 0.0f),
                    fmaxf(blurred.z - original_linear.z, 0.0f));
    const float3 result_linear =
        make_float3(original_linear.x + spill_linear.x * red_gain,
                    original_linear.y + spill_linear.y * green_gain,
                    original_linear.z + spill_linear.z * blue_gain);
    dst[offset] = HalationEncodeDisplay(result_linear, original.w, params);
  }
};

}  // namespace CUDA
}  // namespace alcedo
