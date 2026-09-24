//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/op_kernel.hpp"
#include "edit/runtime/cuda/cuda_film_grain_math.cuh"

namespace alcedo {
namespace CUDA {

GPU_FUNC float FilmGrainSampleAt(const float4* __restrict src, int x, int y, int channel, int width,
                                 int height, size_t pitch_elems, const GPUOperatorParams& params) {
  const int    clamped_x = min(max(x, 0), width - 1);
  const int    clamped_y = min(max(y, 0), height - 1);
  const size_t offset =
      static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x);
  const float4 signal = src[offset];
  // Full-frame renders anchor to full-frame coordinates. ROI preview renders intentionally use the
  // current output buffer coordinates so grain size stays tied to the preview/export resolution.
  const int    ref_x  = params.render_roi_enabled_
                            ? clamped_x
                            : FilmGrainReferenceCoord(
                              clamped_x, width, params.render_roi_x_, params.render_roi_scale_x_,
                              params.render_roi_reference_width_, params.render_roi_enabled_);
  const int    ref_y  = params.render_roi_enabled_
                            ? clamped_y
                            : FilmGrainReferenceCoord(
                              clamped_y, height, params.render_roi_y_, params.render_roi_scale_y_,
                              params.render_roi_reference_height_, params.render_roi_enabled_);
  return FilmGrainSample(FilmGrainChannel(signal, channel), ref_x, ref_y, channel,
                         params.film_grain_.seed_);
}

GPU_FUNC float4 FilmGrainBlurHorizontal(int x, int y, const float4* __restrict src, int width,
                                        int height, size_t pitch_elems,
                                        const GPUOperatorParams& params) {
  float blurred[3] = {};
  for (int channel = 0; channel < 3; ++channel) {
    blurred[channel] = FilmGrainGaussian7(
        FilmGrainSampleAt(src, x, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x - 1, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x + 1, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x - 2, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x + 2, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x - 3, y, channel, width, height, pitch_elems, params),
        FilmGrainSampleAt(src, x + 3, y, channel, width, height, pitch_elems, params));
  }

  return make_float4(blurred[0], blurred[1], blurred[2],
                     FilmGrainReadClamped(src, x, y, width, height, pitch_elems).w);
}

struct GPU_FilmGrainBlurHorizontalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems, GPUOperatorParams& params) const {
    const size_t offset   = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    const auto&  grain    = params.film_grain_;
    const float  strength = fminf(fmaxf(grain.strength_, 0.0f), 1.0f);

    if (!grain.enabled_ || !(strength > 0.0f)) {
      dst[offset] = src[offset];
      return;
    }

    dst[offset] = FilmGrainBlurHorizontal(x, y, src, width, height, pitch_elems, params);
  }
};

struct GPU_FilmGrainApplyVerticalKernel : GPUNeighborOpTag {
  __device__ __forceinline__ void operator()(int x, int y, const float4* __restrict src,
                                             float4* __restrict dst, int width, int height,
                                             size_t pitch_elems, GPUOperatorParams& params) const {
    const size_t offset   = static_cast<size_t>(y) * pitch_elems + static_cast<size_t>(x);
    const auto&  grain    = params.film_grain_;
    const float  strength = fminf(fmaxf(grain.strength_, 0.0f), 1.0f);

    if (!grain.enabled_ || !(strength > 0.0f)) {
      dst[offset] = src[offset];
      return;
    }

    const float4 original = dst[offset];
    const float4 blurred  = FilmGrainBlurVertical(x, y, src, width, height, pitch_elems);
    dst[offset] = FilmGrainApplyDyeClouds(original, blurred, strength);
  }
};

}  // namespace CUDA
}  // namespace alcedo
