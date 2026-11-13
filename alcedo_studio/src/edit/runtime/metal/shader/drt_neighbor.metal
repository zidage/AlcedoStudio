//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <metal_stdlib>
using namespace metal;

constant uint kDrtNeighborBehaviorClarity   = 13u;
constant uint kDrtNeighborBehaviorSharpen   = 14u;
constant uint kDrtNeighborBehaviorHalation  = 15u;
constant uint kDrtNeighborMaxTaps           = 64u;

struct GradeNeighborParams {
  uint  behavior;
  uint  radius;
  uint  tap_count;
  float amount;
  float threshold;
  float weights[kDrtNeighborMaxTaps];
  uint  enabled;
  float sigma_x;
  float sigma_y;
  float redshift[3];
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

/**
 * @brief Horizontal blur of one display-referred DRT/Post neighborhood operator.
 *
 * Writes the separable horizontal result to scratch. Halation uses an exponential
 * window; Clarity and Sharpen use the packed Gaussian weights. Film Grain has its own kernel
 * (film_grain_post.metal).
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
  } else {
    result = DrtGaussianHorizontal(src, position, params);
  }
  dst.write(result, gid);
}

/**
 * @brief Vertical apply of one display-referred DRT/Post neighborhood operator.
 *
 * Reads the unfiltered display image and the horizontal scratch, then writes Sharpen,
 * Clarity, or Halation spill.
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
  }
}
