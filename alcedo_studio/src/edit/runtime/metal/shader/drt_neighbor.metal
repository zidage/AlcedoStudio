//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <metal_stdlib>
using namespace metal;

#include "../../../operators/GPU_kernels/metal_shader/prng.metal"

constant uint kDrtNeighborBehaviorClarity   = 13u;
constant uint kDrtNeighborBehaviorSharpen   = 14u;
constant uint kDrtNeighborBehaviorHalation  = 15u;
constant uint kDrtNeighborBehaviorFilmGrain = 16u;
constant uint kDrtNeighborMaxTaps           = 64u;

struct GradeNeighborParams {
  uint  behavior;
  uint  radius;
  uint  tap_count;
  float amount;
  float threshold;
  float weights[kDrtNeighborMaxTaps];
  uint  enabled;
  uint  seed_lo;
  uint  seed_hi;
  float sigma_x;
  float sigma_y;
  float redshift[3];
  float render_to_reference[6];
  uint  use_reference_coordinates;
  int   reference_width;
  int   reference_height;
};

static inline float4 DrtNeighborRead(texture2d<float, access::read> image, int2 position) {
  const int2 limit = int2(int(image.get_width()) - 1, int(image.get_height()) - 1);
  const int2 clamped =
      int2(clamp(position.x, 0, limit.x), clamp(position.y, 0, limit.y));
  return image.read(uint2(uint(clamped.x), uint(clamped.y)));
}

static inline float DrtNeighborLuma(float3 value) {
  return value.x * 0.114f + value.y * 0.587f + value.z * 0.299f;
}

static inline float DrtNeighborSmoothstep(float edge0, float edge1, float value) {
  const float t = clamp((value - edge0) / max(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline int DrtHalationRadius(float sigma) {
  return !(sigma > 0.0f) ? 0 : clamp(int(ceil(sigma * 3.0f)), 1, int(kDrtNeighborMaxTaps - 1u));
}

static inline float DrtHalationWeight(int tap, float sigma) {
  return tap == 0 ? 1.0f : exp(-float(tap) / max(sigma, 1.0e-6f));
}

static inline float DrtHalationNormalization(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) sum += 2.0f * DrtHalationWeight(tap, sigma);
  return 1.0f / max(sum, 1.0e-6f);
}

static inline float4 DrtGaussianHorizontal(texture2d<float, access::read> src, int2 position,
                                           constant GradeNeighborParams& params) {
  float4 blur = DrtNeighborRead(src, position) * params.weights[0];
  for (uint tap = 1u; tap < params.tap_count; ++tap) {
    const int distance = int(tap);
    blur += (DrtNeighborRead(src, position + int2(distance, 0)) +
             DrtNeighborRead(src, position - int2(distance, 0))) *
            params.weights[tap];
  }
  return blur;
}

static inline float4 DrtGaussianVertical(texture2d<float, access::read> src, int2 position,
                                         constant GradeNeighborParams& params) {
  float4 blur = DrtNeighborRead(src, position) * params.weights[0];
  for (uint tap = 1u; tap < params.tap_count; ++tap) {
    const int distance = int(tap);
    blur += (DrtNeighborRead(src, position + int2(0, distance)) +
             DrtNeighborRead(src, position - int2(0, distance))) *
            params.weights[tap];
  }
  return blur;
}

static inline float4 DrtHalationHorizontal(texture2d<float, access::read> src, int2 position,
                                           constant GradeNeighborParams& params) {
  const int radius   = DrtHalationRadius(params.sigma_x);
  const float norm   = DrtHalationNormalization(radius, params.sigma_x);
  const float4 center = DrtNeighborRead(src, position);
  float4 blur = float4(center.x * norm, center.y * norm, center.z * norm, center.w);
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = DrtHalationWeight(tap, params.sigma_x) * norm;
    blur.xyz += (DrtNeighborRead(src, position - int2(tap, 0)).xyz +
                 DrtNeighborRead(src, position + int2(tap, 0)).xyz) *
                weight;
  }
  return blur;
}

static inline float4 DrtHalationVertical(texture2d<float, access::read> src, int2 position,
                                         constant GradeNeighborParams& params) {
  const int radius = DrtHalationRadius(params.sigma_y);
  const float norm = DrtHalationNormalization(radius, params.sigma_y);
  float4 blur      = DrtNeighborRead(src, position);
  blur.xyz *= norm;
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = DrtHalationWeight(tap, params.sigma_y) * norm;
    blur.xyz += (DrtNeighborRead(src, position - int2(0, tap)).xyz +
                 DrtNeighborRead(src, position + int2(0, tap)).xyz) *
                weight;
  }
  return blur;
}

static inline int2 DrtFilmReferenceCoord(int2 coord, constant GradeNeighborParams& params) {
  if (params.use_reference_coordinates == 0u) return coord;
  const float x        = float(coord.x) + 0.5f;
  const float y        = float(coord.y) + 0.5f;
  const float mapped_x = params.render_to_reference[0] * x + params.render_to_reference[1] * y +
                         params.render_to_reference[2] - 0.5f;
  const float mapped_y = params.render_to_reference[3] * x + params.render_to_reference[4] * y +
                         params.render_to_reference[5] - 0.5f;
  return int2(clamp(int(floor(mapped_x + 0.5f)), 0, max(params.reference_width, 1) - 1),
              clamp(int(floor(mapped_y + 0.5f)), 0, max(params.reference_height, 1) - 1));
}

static inline float DrtFilmChannel(float4 value, int channel) {
  return channel == 0 ? value.x : (channel == 1 ? value.y : value.z);
}

static inline float DrtFilmSample(texture2d<float, access::read> src, int2 coord, int channel,
                                  constant GradeNeighborParams& params) {
  const int2 size = int2(int(src.get_width()), int(src.get_height()));
  coord           = clamp(coord, int2(0), size - int2(1));
  const int2 ref  = DrtFilmReferenceCoord(coord, params);
  const ulong seed =
      (ulong(params.seed_hi) << 32u) | ulong(params.seed_lo);
  const ulong stream = metal_prng_pixel_stream_2d(ref.x, ref.y, uint(channel));
  const float draw   = metal_prng_uniform_float01(seed, stream, 0xd1b54a32d192ed03UL);
  return draw < clamp(DrtFilmChannel(DrtNeighborRead(src, coord), channel), 0.0f, 1.0f) ? 1.0f
                                                                                         : 0.0f;
}

static inline float4 DrtFilmHorizontal(texture2d<float, access::read> src, int2 position,
                                       constant GradeNeighborParams& params) {
  float3 result = float3(0.0f);
  for (int channel = 0; channel < 3; ++channel) {
    float acc = DrtFilmSample(src, position, channel, params) * params.weights[0];
    for (uint tap = 1u; tap < params.tap_count; ++tap) {
      const int distance = int(tap);
      acc += (DrtFilmSample(src, position + int2(-distance, 0), channel, params) +
              DrtFilmSample(src, position + int2(distance, 0), channel, params)) *
             params.weights[tap];
    }
    result[channel] = acc;
  }
  return float4(result, DrtNeighborRead(src, position).w);
}

static inline float4 DrtFilmVertical(texture2d<float, access::read> src, int2 position,
                                     constant GradeNeighborParams& params) {
  float4 blur = DrtNeighborRead(src, position) * params.weights[0];
  for (uint tap = 1u; tap < params.tap_count; ++tap) {
    const int distance = int(tap);
    blur += (DrtNeighborRead(src, position + int2(0, distance)) +
             DrtNeighborRead(src, position - int2(0, distance))) *
            params.weights[tap];
  }
  blur.w = DrtNeighborRead(src, position).w;
  return blur;
}

constant float kDrtFilmRedDensity[11]   = {0.22f, 0.22f, 0.25f, 0.42f, 0.78f, 1.19f,
                                           1.58f, 1.94f, 2.26f, 2.45f, 2.52f};
constant float kDrtFilmRedSigma[11]     = {0.00594f, 0.00565f, 0.00524f, 0.01085f, 0.00844f,
                                           0.00531f, 0.00486f, 0.00486f, 0.00445f, 0.00440f,
                                           0.00474f};
constant float kDrtFilmGreenDensity[11] = {0.59f, 0.61f, 0.66f, 0.94f, 1.36f, 1.76f,
                                           2.18f, 2.49f, 2.61f, 2.67f, 2.69f};
constant float kDrtFilmGreenSigma[11]   = {0.00517f, 0.00524f, 0.00625f, 0.01085f, 0.00823f,
                                           0.00617f, 0.00625f, 0.00691f, 0.00602f, 0.00524f,
                                           0.00445f};
constant float kDrtFilmBlueDensity[11]  = {1.00f, 1.03f, 1.10f, 1.32f, 1.51f, 1.78f,
                                           2.05f, 2.38f, 2.68f, 2.91f, 3.00f};
constant float kDrtFilmBlueSigma[11]    = {0.01185f, 0.01261f, 0.01485f, 0.01581f, 0.01200f,
                                           0.01099f, 0.01127f, 0.01058f, 0.00844f, 0.00641f,
                                           0.00418f};

static inline float DrtFilmLerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float DrtFilmEvalSigma(float density, constant float* density_lut,
                                     constant float* sigma_lut) {
  if (density <= density_lut[0]) return sigma_lut[0];
  for (int i = 0; i < 10; ++i) {
    if (density <= density_lut[i + 1]) {
      const float t =
          (density - density_lut[i]) / max(density_lut[i + 1] - density_lut[i], 1.0e-6f);
      return DrtFilmLerp(sigma_lut[i], sigma_lut[i + 1], t);
    }
  }
  return sigma_lut[10];
}

static inline float DrtFilmGranularity(float signal, int channel) {
  const float u       = clamp(signal, 0.0f, 1.0f);
  const float density = channel == 0   ? DrtFilmLerp(0.22f, 2.52f, u)
                        : channel == 1 ? DrtFilmLerp(0.59f, 2.69f, u)
                                       : DrtFilmLerp(1.00f, 3.00f, u);
  const float sigma   = channel == 0 ? DrtFilmEvalSigma(density, kDrtFilmRedDensity, kDrtFilmRedSigma)
                        : channel == 1
                            ? DrtFilmEvalSigma(density, kDrtFilmGreenDensity, kDrtFilmGreenSigma)
                            : DrtFilmEvalSigma(density, kDrtFilmBlueDensity, kDrtFilmBlueSigma);
  return clamp(sigma / 0.0075f, 0.55f, 2.15f);
}

static inline float DrtFilmDensityError(float signal, float coverage, int channel) {
  return (coverage - clamp(signal, 0.0f, 1.0f)) * DrtFilmGranularity(signal, channel);
}

static inline float4 DrtFilmApply(float4 source, float4 coverage, float amount) {
  float3 density = float3(DrtFilmDensityError(source.x, coverage.x, 0),
                          DrtFilmDensityError(source.y, coverage.y, 1),
                          DrtFilmDensityError(source.z, coverage.z, 2));
  const float neutral = (density.x + density.y + density.z) * (1.0f / 3.0f);
  const float highlight_signal =
      (clamp(source.x, 0.0f, 1.0f) + clamp(source.y, 0.0f, 1.0f) + clamp(source.z, 0.0f, 1.0f)) *
      (1.0f / 3.0f);
  const float highlight      = DrtNeighborSmoothstep(0.72f, 0.96f, highlight_signal);
  const float chroma         = DrtFilmLerp(0.68f, 0.18f, highlight);
  density                    = float3(neutral) + chroma * (density - float3(neutral));
  const float brighten_scale = DrtFilmLerp(1.0f, 0.12f, highlight);
  density.x                  = density.x < 0.0f ? density.x * brighten_scale : density.x;
  density.y                  = density.y < 0.0f ? density.y * brighten_scale : density.y;
  density.z                  = density.z < 0.0f ? density.z * brighten_scale : density.z;
  const float strength       = amount * 1.35f;
  return float4(source.x - source.x * strength * density.x,
                source.y - source.y * strength * density.y,
                source.z - source.z * strength * density.z, source.w);
}

/**
 * @brief Horizontal blur of one display-referred DRT/Post neighborhood operator.
 *
 * Writes the separable horizontal result to scratch. Halation uses an exponential
 * window; film grain samples dye coverage before the Gaussian; Clarity and Sharpen
 * use the packed Gaussian weights.
 */
kernel void drt_neighbor_blur_horizontal(texture2d<float, access::read> src [[texture(0)]],
                                         texture2d<float, access::write> dst [[texture(1)]],
                                         constant GradeNeighborParams& params [[buffer(0)]],
                                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= src.get_width() || gid.y >= src.get_height()) return;
  const int2 position = int2(gid);
  float4 result;
  if (params.behavior == kDrtNeighborBehaviorHalation) {
    result = DrtHalationHorizontal(src, position, params);
  } else if (params.behavior == kDrtNeighborBehaviorFilmGrain) {
    result = DrtFilmHorizontal(src, position, params);
  } else {
    result = DrtGaussianHorizontal(src, position, params);
  }
  dst.write(result, gid);
}

/**
 * @brief Vertical apply of one display-referred DRT/Post neighborhood operator.
 *
 * Reads the unfiltered display image and the horizontal scratch, then writes Sharpen,
 * Clarity, Halation spill, or film-grain dye clouds.
 */
kernel void drt_neighbor_apply_vertical(texture2d<float, access::read> original [[texture(0)]],
                                        texture2d<float, access::read> horizontal [[texture(1)]],
                                        texture2d<float, access::write> dst [[texture(2)]],
                                        constant GradeNeighborParams& params [[buffer(0)]],
                                        uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= original.get_width() || gid.y >= original.get_height()) return;
  const int2 position = int2(gid);
  const float4 source = original.read(gid);
  if (params.behavior == kDrtNeighborBehaviorSharpen) {
    const float4 blur = DrtGaussianVertical(horizontal, position, params);
    float3 high       = source.xyz - blur.xyz;
    if (params.threshold > 0.0f && abs(DrtNeighborLuma(high)) <= params.threshold) {
      high = float3(0.0f);
    }
    dst.write(float4(source.xyz + high * params.amount, source.w), gid);
  } else if (params.behavior == kDrtNeighborBehaviorClarity) {
    const float4 blur       = DrtGaussianVertical(horizontal, position, params);
    const float3 difference = source.xyz - blur.xyz;
    const float protect =
        1.0f - DrtNeighborSmoothstep(0.0f, 0.18f, abs(DrtNeighborLuma(difference)));
    const float centered = (DrtNeighborLuma(source.xyz) - 0.5f) * 2.0f;
    const float strength = params.amount * protect * max(1.0f - centered * centered, 0.0f);
    dst.write(float4(fma(difference, float3(strength), source.xyz), source.w), gid);
  } else if (params.behavior == kDrtNeighborBehaviorHalation) {
    const float4 blur  = DrtHalationVertical(horizontal, position, params);
    const float3 spill = max(blur.xyz - source.xyz, float3(0.0f));
    dst.write(float4(source.xyz + spill * params.amount *
                                      float3(params.redshift[0], params.redshift[1],
                                             params.redshift[2]),
                     source.w),
              gid);
  } else {
    dst.write(DrtFilmApply(source, DrtFilmVertical(horizontal, position, params), params.amount),
              gid);
  }
}
