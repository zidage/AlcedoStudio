//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "common.metal"

constant int kMetalHlsProfileCount = 8;

static inline float metal_wrap_hue(float h) {
  h = fmod(h, 360.0f);
  if (h < 0.0f) {
    h += 360.0f;
  }
  return h;
}

static inline float metal_hue_distance(float a, float b) {
  const float diff = fabs(metal_wrap_hue(a) - metal_wrap_hue(b));
  return fmin(diff, 360.0f - diff);
}

static inline float metal_smoothstep_range(float edge0, float edge1, float x) {
  const float denom = fmax(edge1 - edge0, 1e-6f);
  const float t     = clamp((x - edge0) / denom, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float metal_soft_floor(float x, float floor, float softness) {
  const float t = (x - floor) / fmax(softness, 1e-6f);
  if (t > 20.0f) {
    return x;
  }
  if (t < -20.0f) {
    return floor;
  }
  return floor + softness * log(1.0f + exp(t));
}

static inline float metal_hls_acescc_decode(float acescc) {
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
    return (exp2(acescc * kB - kA) - kDenormOffset) * 2.0f;
  }
  return exp2(acescc * kB - kA);
}

static inline float metal_hls_acescc_encode(float linear_ap1) {
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
    return (log2(kDenormOffset + linear_ap1 * 0.5f) + kA) / kB;
  }
  return (log2(linear_ap1) + kA) / kB;
}

static inline float3 metal_hls_acescc_to_ap1(float3 acescc) {
  return float3(metal_hls_acescc_decode(acescc.x), metal_hls_acescc_decode(acescc.y),
                metal_hls_acescc_decode(acescc.z));
}

static inline float3 metal_hls_ap1_to_acescc(float3 ap1) {
  return float3(metal_hls_acescc_encode(ap1.x), metal_hls_acescc_encode(ap1.y),
                metal_hls_acescc_encode(ap1.z));
}

static inline float metal_hls_fast_cbrt(float x) {
  if (x == 0.0f) {
    return 0.0f;
  }

  const float ax = fabs(x);
  uint bits = as_type<uint>(ax);
  bits = bits / 3u + 0x2a5119f2u;
  float y = as_type<float>(bits);
  y = (2.0f * y + ax / (y * y)) * (1.0f / 3.0f);
  y = (2.0f * y + ax / (y * y)) * (1.0f / 3.0f);
  return (x < 0.0f) ? -y : y;
}

static inline float3 metal_hls_ap1_to_oklab(float3 ap1) {
  const float l = 0.62217537f * ap1.x + 0.34268438f * ap1.y + 0.02339492f * ap1.z;
  const float m = 0.26593478f * ap1.x + 0.62930460f * ap1.y + 0.10828100f * ap1.z;
  const float s = 0.09725037f * ap1.x + 0.18525749f * ap1.y + 0.77254586f * ap1.z;

  const float l_ = metal_hls_fast_cbrt(l);
  const float m_ = metal_hls_fast_cbrt(m);
  const float s_ = metal_hls_fast_cbrt(s);

  return float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static inline float3 metal_hls_oklab_to_ap1(float3 lab) {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;

  const float l = l_ * l_ * l_;
  const float m = m_ * m_ * m_;
  const float s = s_ * s_ * s_;

  return float3(2.09085732f * l - 1.16812363f * m + 0.10040848f * s,
                -0.87435428f * l + 2.14592958f * m - 0.27429822f * s,
                -0.05353206f * l - 0.36754978f * m + 1.34755888f * s);
}

static inline float3 metal_hls_fit_ap1_lower_gamut(float3 adjusted_ap1, float3 neutral_ap1) {
  constexpr float kLower = -1e-5f;
  float           scale  = 1.0f;

  if (adjusted_ap1.x < kLower && neutral_ap1.x > adjusted_ap1.x) {
    scale = fmin(scale, (neutral_ap1.x - kLower) / (neutral_ap1.x - adjusted_ap1.x));
  }
  if (adjusted_ap1.y < kLower && neutral_ap1.y > adjusted_ap1.y) {
    scale = fmin(scale, (neutral_ap1.y - kLower) / (neutral_ap1.y - adjusted_ap1.y));
  }
  if (adjusted_ap1.z < kLower && neutral_ap1.z > adjusted_ap1.z) {
    scale = fmin(scale, (neutral_ap1.z - kLower) / (neutral_ap1.z - adjusted_ap1.z));
  }

  scale = clamp(scale, 0.0f, 1.0f);
  return neutral_ap1 + (adjusted_ap1 - neutral_ap1) * scale;
}

#include "tone_mapping.metal"

static inline float4 GPU_TintOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.tint_enabled_ == 0u) {
    return px;
  }

  px.y += params.tint_offset_;
  return px;
}

static inline float4 GPU_VibranceOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.vibrance_enabled_ == 0u) {
    return px;
  }

  const float max_val = fmax(fmax(px.x, px.y), px.z);
  const float min_val = fmin(fmin(px.x, px.y), px.z);
  const float chroma  = max_val - min_val;
  const float strength = params.vibrance_offset_;
  const float falloff  = exp(-3.0f * chroma);
  const float scale    = 1.0f + strength * falloff;

  if (params.vibrance_offset_ >= 0.0f) {
    const float luma = px.x * 0.299f + px.y * 0.587f + px.z * 0.114f;
    px.x             = luma + (px.x - luma) * scale;
    px.y             = luma + (px.y - luma) * scale;
    px.z             = luma + (px.z - luma) * scale;
  } else {
    const float avg = (px.x + px.y + px.z) / 3.0f;
    px.x += (avg - px.x) * (1.0f - scale);
    px.y += (avg - px.y) * (1.0f - scale);
    px.z += (avg - px.z) * (1.0f - scale);
  }
  return px;
}

static inline float4 GPU_ColorWheelOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.color_wheel_enabled_ == 0u) {
    return px;
  }

  constexpr float kEps = 1e-6f;
  const float offset_r = params.lift_color_offset_[0] + params.lift_luminance_offset_;
  const float offset_g = params.lift_color_offset_[1] + params.lift_luminance_offset_;
  const float offset_b = params.lift_color_offset_[2] + params.lift_luminance_offset_;

  const float slope_r  = fmax(params.gain_color_offset_[0] + params.gain_luminance_offset_, kEps);
  const float slope_g  = fmax(params.gain_color_offset_[1] + params.gain_luminance_offset_, kEps);
  const float slope_b  = fmax(params.gain_color_offset_[2] + params.gain_luminance_offset_, kEps);

  const float power_r  = fmax(params.gamma_color_offset_[0] + params.gamma_luminance_offset_, kEps);
  const float power_g  = fmax(params.gamma_color_offset_[1] + params.gamma_luminance_offset_, kEps);
  const float power_b  = fmax(params.gamma_color_offset_[2] + params.gamma_luminance_offset_, kEps);

  const float base_r   = fmax(px.x * slope_r + offset_r, 0.0f);
  const float base_g   = fmax(px.y * slope_g + offset_g, 0.0f);
  const float base_b   = fmax(px.z * slope_b + offset_b, 0.0f);

  px.x                 = pow(base_r, power_r);
  px.y                 = pow(base_g, power_g);
  px.z                 = pow(base_b, power_b);
  return px;
}

static inline float4 GPU_HLSOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.hls_enabled_ == 0u && params.saturation_enabled_ == 0u) {
    return px;
  }

  constexpr float kEps = 1e-6f;
  constexpr float kPi  = 3.14159265358979323846f;
  const float3 source_ap1    = metal_hls_acescc_to_ap1(px.xyz);
  const float3 source_lab    = metal_hls_ap1_to_oklab(source_ap1);
  const float  source_chroma = length(source_lab.yz);
  if (source_chroma <= kEps) {
    return px;
  }
  const float source_hue = metal_wrap_hue(atan2(source_lab.z, source_lab.y) * (180.0f / kPi));

  float3 curve = float3(0.0f);
  if (params.hls_enabled_ != 0u) {
    int profile_count = params.hls_profile_count_;
    if (profile_count < 1) {
      profile_count = 1;
    }
    if (profile_count > kMetalHlsProfileCount) {
      profile_count = kMetalHlsProfileCount;
    }

    float accum_h = 0.0f;
    float accum_l = 0.0f;
    float accum_c = 0.0f;
    float accum_weight = 0.0f;
    int   nearest = 0;
    float nearest_dist = metal_hue_distance(source_hue, metal_wrap_hue(params.hls_profile_hues_[0]));

    for (int i = 0; i < kMetalHlsProfileCount; ++i) {
      if (i >= profile_count) {
        continue;
      }

      const float width     = fmax(params.hls_profile_hue_ranges_[i], 1.0f);
      const float target_h  = metal_wrap_hue(params.hls_profile_hues_[i]);
      const float hue_dist  = metal_hue_distance(source_hue, target_h);
      if (hue_dist < nearest_dist) {
        nearest_dist = hue_dist;
        nearest      = i;
      }

      const float t      = hue_dist / width;
      const float weight = exp2(-(t * t));
      accum_h += params.hls_profile_adjustments_[i][0] * weight;
      accum_l += params.hls_profile_adjustments_[i][1] * weight;
      accum_c += params.hls_profile_adjustments_[i][2] * weight;
      accum_weight += weight;
    }

    curve = float3(params.hls_profile_adjustments_[nearest][0],
                   params.hls_profile_adjustments_[nearest][1],
                   params.hls_profile_adjustments_[nearest][2]);
    if (accum_weight > kEps) {
      curve = float3(accum_h, accum_l, accum_c) / accum_weight;
    }
  }
  const float saturation_scale =
      (params.saturation_enabled_ != 0u) ? fmax(params.saturation_offset_, 0.0f) : 1.0f;
  if (fabs(curve.x) <= kEps && fabs(curve.y) <= kEps && fabs(curve.z) <= kEps &&
      fabs(saturation_scale - 1.0f) <= kEps) {
    return px;
  }

  const bool  has_curve = fabs(curve.x) > kEps || fabs(curve.y) > kEps ||
                          fabs(curve.z) > kEps;
  float       protection = 0.0f;
  if (has_curve) {
    const float chroma_confidence    = metal_smoothstep_range(0.005f, 0.030f, source_chroma);
    const float shadow_confidence    = metal_smoothstep_range(0.005f, 0.050f, source_lab.x);
    const float highlight_confidence = 1.0f - metal_smoothstep_range(1.35f, 2.25f, source_lab.x);
    protection = clamp(chroma_confidence * shadow_confidence * highlight_confidence, 0.0f, 1.0f);
  }
  if (protection <= kEps && fabs(saturation_scale - 1.0f) <= kEps) {
    return px;
  }

  constexpr float kCurveGain = 2.25f;
  const float adjusted_hue_rad =
      metal_wrap_hue(source_hue + curve.x * kCurveGain * protection) * (kPi / 180.0f);
  const float adjusted_lightness =
      (fabs(curve.y) > kEps && protection > kEps)
          ? metal_soft_floor(source_lab.x + curve.y * kCurveGain * 0.5f * protection, 0.0f,
                             0.02f)
          : source_lab.x;
  const float chroma_strength = (curve.z >= 0.0f) ? 4.5f : 3.25f;
  const float adjusted_chroma =
      source_chroma * saturation_scale *
      exp2(curve.z * kCurveGain * chroma_strength * protection);

  const float3 adjusted_lab =
      float3(adjusted_lightness, adjusted_chroma * cos(adjusted_hue_rad),
             adjusted_chroma * sin(adjusted_hue_rad));
  const float3 neutral_lab = float3(adjusted_lightness, 0.0f, 0.0f);
  const float3 output_ap1 =
      metal_hls_fit_ap1_lower_gamut(metal_hls_oklab_to_ap1(adjusted_lab),
                                    metal_hls_oklab_to_ap1(neutral_lab));
  px.xyz = metal_hls_ap1_to_acescc(output_ap1);
  return px;
}
