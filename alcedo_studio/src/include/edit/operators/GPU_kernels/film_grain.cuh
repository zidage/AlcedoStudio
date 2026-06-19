//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda/prng.hpp"
#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/op_kernel.hpp"

namespace alcedo {
namespace CUDA {

GPU_FUNC int FilmGrainReferenceCoord(int coord, int length, int roi_origin, float roi_scale,
                                     int reference_length, bool roi_enabled) {
  const float safe_length = fmaxf(static_cast<float>(length), 1.0f);
  const int   full_extent = max(reference_length > 0 ? reference_length : length, 1);
  const float full_length = static_cast<float>(full_extent);
  const float origin      = roi_enabled ? static_cast<float>(roi_origin) : 0.0f;
  const float span        = roi_enabled ? fmaxf(roi_scale * full_length, 1.0f) : full_length;
  const float mapped = origin + ((static_cast<float>(coord) + 0.5f) * span / safe_length) - 0.5f;
  return min(max(static_cast<int>(floorf(mapped + 0.5f)), 0), full_extent - 1);
}

GPU_FUNC float FilmGrainChannel(float4 value, int channel) {
  if (channel == 0) {
    return value.x;
  }
  if (channel == 1) {
    return value.y;
  }
  return value.z;
}

GPU_FUNC float4 FilmGrainReadClamped(const float4* __restrict src, int x, int y, int width,
                                     int height, size_t pitch_elems) {
  const int clamped_x = min(max(x, 0), width - 1);
  const int clamped_y = min(max(y, 0), height - 1);
  return src[static_cast<size_t>(clamped_y) * pitch_elems + static_cast<size_t>(clamped_x)];
}

GPU_FUNC float FilmGrainClampProbability(float value) { return fminf(fmaxf(value, 0.0f), 1.0f); }

GPU_FUNC float FilmGrainLerp(float a, float b, float t) { return a + (b - a) * t; }

GPU_FUNC float FilmGrainEvalDensitySigma(float density, const float* density_lut,
                                         const float* sigma_lut, int count) {
  if (density <= density_lut[0]) {
    return sigma_lut[0];
  }
  for (int i = 0; i < count - 1; ++i) {
    const float lo = density_lut[i];
    const float hi = density_lut[i + 1];
    if (density <= hi) {
      const float t = (density - lo) / fmaxf(hi - lo, 1.0e-6f);
      return FilmGrainLerp(sigma_lut[i], sigma_lut[i + 1], t);
    }
  }
  return sigma_lut[count - 1];
}

GPU_FUNC float FilmGrainLayerDensity(float signal, int channel) {
  const float u = FilmGrainClampProbability(signal);
  if (channel == 0) {
    return FilmGrainLerp(0.22f, 2.52f, u);
  }
  if (channel == 1) {
    return FilmGrainLerp(0.59f, 2.69f, u);
  }
  return FilmGrainLerp(1.00f, 3.00f, u);
}

GPU_FUNC float FilmGrainDatasheetSigmaD(float density, int channel) {
  constexpr int   kCount = 11;
  constexpr float kRedDensity[kCount] = {0.22f, 0.22f, 0.25f, 0.42f, 0.78f, 1.19f,
                                         1.58f, 1.94f, 2.26f, 2.45f, 2.52f};
  constexpr float kRedSigma[kCount]   = {0.00594f, 0.00565f, 0.00524f, 0.01085f, 0.00844f,
                                         0.00531f, 0.00486f, 0.00486f, 0.00445f, 0.00440f,
                                         0.00474f};
  constexpr float kGreenDensity[kCount] = {0.59f, 0.61f, 0.66f, 0.94f, 1.36f, 1.76f,
                                           2.18f, 2.49f, 2.61f, 2.67f, 2.69f};
  constexpr float kGreenSigma[kCount]   = {0.00517f, 0.00524f, 0.00625f, 0.01085f, 0.00823f,
                                           0.00617f, 0.00625f, 0.00691f, 0.00602f, 0.00524f,
                                           0.00445f};
  constexpr float kBlueDensity[kCount] = {1.00f, 1.03f, 1.10f, 1.32f, 1.51f, 1.78f,
                                          2.05f, 2.38f, 2.68f, 2.91f, 3.00f};
  constexpr float kBlueSigma[kCount]   = {0.01185f, 0.01261f, 0.01485f, 0.01581f, 0.01200f,
                                          0.01099f, 0.01127f, 0.01058f, 0.00844f, 0.00641f,
                                          0.00418f};

  if (channel == 0) {
    return FilmGrainEvalDensitySigma(density, kRedDensity, kRedSigma, kCount);
  }
  if (channel == 1) {
    return FilmGrainEvalDensitySigma(density, kGreenDensity, kGreenSigma, kCount);
  }
  return FilmGrainEvalDensitySigma(density, kBlueDensity, kBlueSigma, kCount);
}

GPU_FUNC float FilmGrainDatasheetGranularityScale(float signal, int channel) {
  constexpr float kReferenceSigmaD = 0.0075f;
  const float     density          = FilmGrainLayerDensity(signal, channel);
  const float     sigma            = FilmGrainDatasheetSigmaD(density, channel);
  return fminf(fmaxf(sigma / kReferenceSigmaD, 0.55f), 2.15f);
}

GPU_FUNC float FilmGrainDyeDensityError(float transmittance, float layer_coverage, int channel) {
  const float expected_coverage = FilmGrainClampProbability(transmittance);
  const float density_error     =
      (layer_coverage - expected_coverage) * FilmGrainDatasheetGranularityScale(transmittance,
                                                                                channel);
  return density_error;
}

GPU_FUNC float FilmGrainApplyDyeDensity(float transmittance, float density_error, float strength) {
  return transmittance - transmittance * strength * density_error;
}

GPU_FUNC float FilmGrainSmoothstep(float edge0, float edge1, float value) {
  const float t = fminf(fmaxf((value - edge0) / fmaxf(edge1 - edge0, 1.0e-6f), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

GPU_FUNC float FilmGrainCompressHighlightBrightening(float density_error, float highlight_weight) {
  const float brighten_scale = FilmGrainLerp(1.0f, 0.12f, highlight_weight);
  return density_error < 0.0f ? density_error * brighten_scale : density_error;
}

GPU_FUNC float4 FilmGrainApplyDyeClouds(float4 transmittance, float4 layer_coverage,
                                        float strength) {
  constexpr float kColorDyeChroma = 0.68f;
  constexpr float kDyeDensityBoost = 1.35f;
  constexpr float kHighlightDyeChroma = 0.18f;

  const float red_density =
      FilmGrainDyeDensityError(transmittance.x, layer_coverage.x, 0);
  const float green_density =
      FilmGrainDyeDensityError(transmittance.y, layer_coverage.y, 1);
  const float blue_density =
      FilmGrainDyeDensityError(transmittance.z, layer_coverage.z, 2);
  const float neutral_density = (red_density + green_density + blue_density) * (1.0f / 3.0f);
  const float highlight_signal =
      (FilmGrainClampProbability(transmittance.x) + FilmGrainClampProbability(transmittance.y) +
       FilmGrainClampProbability(transmittance.z)) *
      (1.0f / 3.0f);
  const float highlight_weight = FilmGrainSmoothstep(0.72f, 0.96f, highlight_signal);
  const float color_chroma =
      FilmGrainLerp(kColorDyeChroma, kHighlightDyeChroma, highlight_weight);

  const float red_balanced =
      FilmGrainCompressHighlightBrightening(
          neutral_density + color_chroma * (red_density - neutral_density), highlight_weight);
  const float green_balanced =
      FilmGrainCompressHighlightBrightening(
          neutral_density + color_chroma * (green_density - neutral_density), highlight_weight);
  const float blue_balanced =
      FilmGrainCompressHighlightBrightening(
          neutral_density + color_chroma * (blue_density - neutral_density), highlight_weight);
  const float boosted_strength = strength * kDyeDensityBoost;

  return make_float4(FilmGrainApplyDyeDensity(transmittance.x, red_balanced, boosted_strength),
                     FilmGrainApplyDyeDensity(transmittance.y, green_balanced, boosted_strength),
                     FilmGrainApplyDyeDensity(transmittance.z, blue_balanced, boosted_strength),
                     transmittance.w);
}

GPU_FUNC float FilmGrainSample(float probability, int ref_x, int ref_y, int channel,
                               std::uint64_t seed) {
  const std::uint64_t stream =
      cuda::PixelStream2D(ref_x, ref_y, static_cast<std::uint32_t>(channel));
  const float draw = cuda::UniformFloat01(seed, stream, 0xd1b54a32d192ed03ULL);
  return draw < FilmGrainClampProbability(probability) ? 1.0f : 0.0f;
}

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

GPU_FUNC float FilmGrainGaussian7(float c0, float n1, float p1, float n2, float p2, float n3,
                                  float p3) {
  constexpr float kW0 = 0.49867642f;
  constexpr float kW1 = 0.22831073f;
  constexpr float kW2 = 0.02192964f;
  constexpr float kW3 = 0.00042142f;
  return c0 * kW0 + (n1 + p1) * kW1 + (n2 + p2) * kW2 + (n3 + p3) * kW3;
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

GPU_FUNC float4 FilmGrainBlurVertical(int x, int y, const float4* __restrict src, int width,
                                      int height, size_t pitch_elems) {
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
