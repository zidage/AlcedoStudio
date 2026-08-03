//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of color adjustment operators

#pragma once

#include <array>
#include <cstdio>
#include <vector>

#include <cuda_runtime.h>
#include <device_types.h>

#include "edit/operators/op_kernel.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "param.cuh"

#define GPU_HD_FUNC __host__ __device__ __forceinline__

namespace alcedo {
namespace CUDA {

GPU_FUNC float hls_oklch_wrap_hue(float h) {
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;
  return h;
}

GPU_FUNC float hls_oklch_hue_distance(float a, float b) {
  const float diff = fabsf(hls_oklch_wrap_hue(a) - hls_oklch_wrap_hue(b));
  return fminf(diff, 360.0f - diff);
}

GPU_HD_FUNC float hls_oklch_smoothstep(float edge0, float edge1, float x) {
  const float denom = fmaxf(edge1 - edge0, 1e-6f);
  const float t     = fminf(fmaxf((x - edge0) / denom, 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

GPU_FUNC float hls_oklch_hue_selection_weight(float hue_distance, float range) {
  const float half_weight_radius = fmaxf(range, 1.0f);
  const float t                  = hue_distance / half_weight_radius;
  return exp2f(-(t * t));
}

GPU_FUNC float hls_oklch_soft_floor(float x, float floor, float softness) {
  const float t = (x - floor) / fmaxf(softness, 1e-6f);
  if (t > 20.0f) return x;
  if (t < -20.0f) return floor;
  return floor + softness * logf(1.0f + expf(t));
}

GPU_FUNC float hls_oklch_acescc_decode(float acescc) {
  constexpr float kLog2Min      = -15.0f;
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor     = (kLog2Denorm + kA) / kB;
  const float denorm_threshold = (kLog2Min + kA) / kB;
  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    return (exp2f(acescc * kB - kA) - kDenormOffset) * 2.0f;
  }
  return exp2f(acescc * kB - kA);
}

GPU_FUNC float hls_oklch_acescc_encode(float linear_ap1) {
  constexpr float kLog2Denorm   = -16.0f;
  constexpr float kDenormTrans  = 0.00003051757812f;
  constexpr float kDenormOffset = 0.00001525878906f;
  constexpr float kA            = 9.72f;
  constexpr float kB            = 17.52f;

  const float encode_floor = (kLog2Denorm + kA) / kB;
  if (linear_ap1 <= 0.0f) {
    return encode_floor + linear_ap1;
  }
  if (linear_ap1 < kDenormTrans) {
    return (log2f(kDenormOffset + linear_ap1 * 0.5f) + kA) / kB;
  }
  return (log2f(linear_ap1) + kA) / kB;
}

GPU_FUNC float3 hls_oklch_acescc_to_ap1(float3 acescc) {
  return make_float3(hls_oklch_acescc_decode(acescc.x), hls_oklch_acescc_decode(acescc.y),
                     hls_oklch_acescc_decode(acescc.z));
}

GPU_FUNC float3 hls_oklch_ap1_to_acescc(float3 ap1) {
  return make_float3(hls_oklch_acescc_encode(ap1.x), hls_oklch_acescc_encode(ap1.y),
                     hls_oklch_acescc_encode(ap1.z));
}

GPU_FUNC float3 hls_oklch_ap1_to_oklab(float3 ap1) {
  const float l = 0.62217537f * ap1.x + 0.34268438f * ap1.y + 0.02339492f * ap1.z;
  const float m = 0.26593478f * ap1.x + 0.62930460f * ap1.y + 0.10828100f * ap1.z;
  const float s = 0.09725037f * ap1.x + 0.18525749f * ap1.y + 0.77254586f * ap1.z;

  const float l_ = cbrtf(l);
  const float m_ = cbrtf(m);
  const float s_ = cbrtf(s);

  return make_float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                     1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                     0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

GPU_FUNC float3 hls_oklch_oklab_to_ap1(float3 lab) {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;

  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  return make_float3(2.09085732f * l - 1.16812363f * m + 0.10040848f * s,
                     -0.87435428f * l + 2.14592958f * m - 0.27429822f * s,
                     -0.05353206f * l - 0.36754978f * m + 1.34755888f * s);
}

GPU_FUNC float3 hls_oklch_fit_ap1_lower_gamut(float3 adjusted_ap1, float3 neutral_ap1) {
  constexpr float kLower = -1e-5f;
  float           scale  = 1.0f;

  if (adjusted_ap1.x < kLower && neutral_ap1.x > adjusted_ap1.x) {
    scale = fminf(scale, (neutral_ap1.x - kLower) / (neutral_ap1.x - adjusted_ap1.x));
  }
  if (adjusted_ap1.y < kLower && neutral_ap1.y > adjusted_ap1.y) {
    scale = fminf(scale, (neutral_ap1.y - kLower) / (neutral_ap1.y - adjusted_ap1.y));
  }
  if (adjusted_ap1.z < kLower && neutral_ap1.z > adjusted_ap1.z) {
    scale = fminf(scale, (neutral_ap1.z - kLower) / (neutral_ap1.z - adjusted_ap1.z));
  }

  scale = fminf(fmaxf(scale, 0.0f), 1.0f);
  return make_float3(neutral_ap1.x + (adjusted_ap1.x - neutral_ap1.x) * scale,
                     neutral_ap1.y + (adjusted_ap1.y - neutral_ap1.y) * scale,
                     neutral_ap1.z + (adjusted_ap1.z - neutral_ap1.z) * scale);
}

#include "edit/operators/GPU_kernels/tone_mapping.cuh"

GPU_FUNC float3 hls_oklch_evaluate_hue_curve(float hue, GPUOperatorParams& params,
                                             int profile_count) {
  constexpr float kEps = 1e-6f;
  float           sum_h = 0.0f;
  float           sum_l = 0.0f;
  float           sum_c = 0.0f;
  float           sum_weight = 0.0f;
  int             nearest = 0;
  float           nearest_dist =
      hls_oklch_hue_distance(hue, hls_oklch_wrap_hue(params.hls_profile_hues_[0]));

#pragma unroll
  for (int i = 0; i < OperatorParams::kHlsProfileCount; ++i) {
    if (i >= profile_count) {
      continue;
    }

    const float target_h = hls_oklch_wrap_hue(params.hls_profile_hues_[i]);
    const float hue_dist = hls_oklch_hue_distance(hue, target_h);
    if (hue_dist < nearest_dist) {
      nearest_dist = hue_dist;
      nearest      = i;
    }

    const float width  = fmaxf(params.hls_profile_hue_ranges_[i], 1.0f);
    const float weight = hls_oklch_hue_selection_weight(hue_dist, width);
    sum_h += params.hls_profile_adjustments_[i][0] * weight;
    sum_l += params.hls_profile_adjustments_[i][1] * weight;
    sum_c += params.hls_profile_adjustments_[i][2] * weight;
    sum_weight += weight;
  }

  if (sum_weight <= kEps) {
    return make_float3(params.hls_profile_adjustments_[nearest][0],
                       params.hls_profile_adjustments_[nearest][1],
                       params.hls_profile_adjustments_[nearest][2]);
  }

  const float inv_weight = 1.0f / sum_weight;
  return make_float3(sum_h * inv_weight, sum_l * inv_weight, sum_c * inv_weight);
}

struct GPU_HLSOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.hls_enabled_ && !params.saturation_enabled_) return;

    const float kEps = 1e-6f;
    const float kPi  = 3.14159265358979323846f;
    const float3 source_acescc = make_float3(p->x, p->y, p->z);
    const float3 source_ap1    = hls_oklch_acescc_to_ap1(source_acescc);
    const float3 source_lab    = hls_oklch_ap1_to_oklab(source_ap1);
    const float  source_chroma = hypotf(source_lab.y, source_lab.z);
    if (source_chroma <= kEps) {
      return;
    }
    const float source_hue =
        hls_oklch_wrap_hue(atan2f(source_lab.z, source_lab.y) * (180.0f / kPi));

    float3 curve = make_float3(0.0f, 0.0f, 0.0f);
    if (params.hls_enabled_) {
      int profile_count = params.hls_profile_count_;
      if (profile_count < 1) {
        profile_count = 1;
      }
      if (profile_count > OperatorParams::kHlsProfileCount) {
        profile_count = OperatorParams::kHlsProfileCount;
      }

      curve = hls_oklch_evaluate_hue_curve(source_hue, params, profile_count);
    }
    const float saturation_scale =
        params.saturation_enabled_ ? fmaxf(params.saturation_offset_, 0.0f) : 1.0f;
    if (fabsf(curve.x) <= kEps && fabsf(curve.y) <= kEps && fabsf(curve.z) <= kEps &&
        fabsf(saturation_scale - 1.0f) <= kEps) {
      return;
    }

    const bool  has_curve = fabsf(curve.x) > kEps || fabsf(curve.y) > kEps ||
                            fabsf(curve.z) > kEps;
    float       protection = 0.0f;
    if (has_curve) {
      const float chroma_confidence    = hls_oklch_smoothstep(0.005f, 0.030f, source_chroma);
      const float shadow_confidence    = hls_oklch_smoothstep(0.005f, 0.050f, source_lab.x);
      const float highlight_confidence = 1.0f - hls_oklch_smoothstep(1.35f, 2.25f, source_lab.x);
      protection =
          fminf(fmaxf(chroma_confidence * shadow_confidence * highlight_confidence, 0.0f), 1.0f);
    }
    if (protection <= kEps && fabsf(saturation_scale - 1.0f) <= kEps) {
      return;
    }

    constexpr float kCurveGain = 2.25f;
    const float adjusted_hue_rad =
        hls_oklch_wrap_hue(source_hue + curve.x * kCurveGain * protection) * (kPi / 180.0f);
    const float adjusted_lightness =
        (fabsf(curve.y) > kEps && protection > kEps)
            ? hls_oklch_soft_floor(source_lab.x + curve.y * kCurveGain * 0.5f * protection, 0.0f,
                                   0.02f)
            : source_lab.x;
    const float chroma_strength = (curve.z >= 0.0f) ? 4.5f : 3.25f;
    const float adjusted_chroma =
        source_chroma * saturation_scale *
        exp2f(curve.z * kCurveGain * chroma_strength * protection);

    const float3 adjusted_lab =
        make_float3(adjusted_lightness, adjusted_chroma * cosf(adjusted_hue_rad),
                    adjusted_chroma * sinf(adjusted_hue_rad));
    const float3 neutral_lab  = make_float3(adjusted_lightness, 0.0f, 0.0f);
    const float3 neutral_ap1  = hls_oklch_oklab_to_ap1(neutral_lab);
    const float3 output_ap1 =
        hls_oklch_fit_ap1_lower_gamut(hls_oklch_oklab_to_ap1(adjusted_lab), neutral_ap1);
    const float3 output_acescc = hls_oklch_ap1_to_acescc(output_ap1);

    p->x = output_acescc.x;
    p->y = output_acescc.y;
    p->z = output_acescc.z;
  }
};

struct GPU_TintOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.tint_enabled_) return;

    p->y += params.tint_offset_;
  }
};

struct GPU_VibranceOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.vibrance_enabled_) return;

    float max_val  = fmaxf(fmaxf(p->x, p->y), p->z);
    float min_val  = fminf(fminf(p->x, p->y), p->z);
    float chroma   = max_val - min_val;

    // chroma in [0, max], vibrance_offset in [-100, 100]
    float strength = params.vibrance_offset_;

    // Protect already highly saturated color
    float falloff  = expf(-3.0f * chroma);

    float scale    = 1.0f + strength * falloff;

    if (params.vibrance_offset_ >= 0.0f) {
      float luma = p->x * 0.299f + p->y * 0.587f + p->z * 0.114f;

      p->x          = luma + (p->x - luma) * scale;
      p->y          = luma + (p->y - luma) * scale;
      p->z          = luma + (p->z - luma) * scale;

    } else {
      float avg = (p->x + p->y + p->z) / 3.0f;
      p->x += (avg - p->x) * (1.0f - scale);
      p->y += (avg - p->y) * (1.0f - scale);
      p->z += (avg - p->z) * (1.0f - scale);
    }
  }
};

struct GPU_ColorWheelOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.color_wheel_enabled_) return;

    constexpr float kEps = 1e-6f;

    const float offset_r = params.lift_color_offset_[0] + params.lift_luminance_offset_;
    const float offset_g = params.lift_color_offset_[1] + params.lift_luminance_offset_;
    const float offset_b = params.lift_color_offset_[2] + params.lift_luminance_offset_;

    const float slope_r  = fmaxf(params.gain_color_offset_[0] + params.gain_luminance_offset_, kEps);
    const float slope_g  = fmaxf(params.gain_color_offset_[1] + params.gain_luminance_offset_, kEps);
    const float slope_b  = fmaxf(params.gain_color_offset_[2] + params.gain_luminance_offset_, kEps);

    const float power_r  =
        fmaxf(params.gamma_color_offset_[0] + params.gamma_luminance_offset_, kEps);
    const float power_g  =
        fmaxf(params.gamma_color_offset_[1] + params.gamma_luminance_offset_, kEps);
    const float power_b  =
        fmaxf(params.gamma_color_offset_[2] + params.gamma_luminance_offset_, kEps);

    const float base_r = fmaxf(p->x * slope_r + offset_r, 0.0f);
    const float base_g = fmaxf(p->y * slope_g + offset_g, 0.0f);
    const float base_b = fmaxf(p->z * slope_b + offset_b, 0.0f);

    // p->x               = fminf(fmaxf(powf(base_r, power_r), 0.0f), 1.0f);
    // p->y               = fminf(fmaxf(powf(base_g, power_g), 0.0f), 1.0f);
    // p->z               = fminf(fmaxf(powf(base_b, power_b), 0.0f), 1.0f);
    p->x               = powf(base_r, power_r);
    p->y               = powf(base_g, power_g);
    p->z               = powf(base_b, power_b);
  }
};
};  // namespace CUDA
};  // namespace alcedo
