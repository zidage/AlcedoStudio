//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "common.metal"

static inline float metal_sigmoid(float t) { return 1.0f / (1.0f + exp(-t)); }

static inline float metal_contrast_sigmoid_01(float x, float k) {
  const float a = metal_sigmoid(-0.5f * k);
  const float b = metal_sigmoid(0.5f * k);
  const float y = metal_sigmoid(k * (x - 0.5f));
  return (y - a) / (b - a);
}

static inline float3 metal_contrast_on_luma_acescc(float3 rgb_acescc, float k = 6.0f,
                                                   float pivot = 0.5f, float range = 0.35f,
                                                   float eps = 1e-6f) {
  const float Y = 0.2126f * rgb_acescc.x + 0.7152f * rgb_acescc.y + 0.0722f * rgb_acescc.z;
  const float lo = pivot - range;
  const float hi = pivot + range;
  const float t  = (Y - lo) / (hi - lo);
  if (t <= 0.0f || t >= 1.0f) {
    return rgb_acescc;
  }

  const float t2    = metal_contrast_sigmoid_01(t, k);
  const float Y2    = lo + (hi - lo) * t2;
  const float scale = Y2 / fmax(Y, eps);
  return rgb_acescc * scale;
}

static inline float metal_luma(float3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

static inline float metal_shared_tone_luma(float3 rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

static inline float metal_evaluate_shared_tone_curve(float x, constant MetalFusedParams& params) {
  const int curve_count = params.shared_tone_curve_ctrl_pts_size_;
  if (curve_count <= 0) {
    return x;
  }
  if (curve_count == 1) {
    return params.shared_tone_curve_ctrl_pts_y_[0];
  }
  if (x <= params.shared_tone_curve_ctrl_pts_x_[0]) {
    return params.shared_tone_curve_ctrl_pts_y_[0];
  }
  if (x >= params.shared_tone_curve_ctrl_pts_x_[curve_count - 1]) {
    return params.shared_tone_curve_ctrl_pts_y_[curve_count - 1] +
           (x - params.shared_tone_curve_ctrl_pts_x_[curve_count - 1]) *
               params.shared_tone_curve_m_[curve_count - 1];
  }

  int idx = curve_count - 2;
  for (int i = 0; i < curve_count - 1; ++i) {
    if (x < params.shared_tone_curve_ctrl_pts_x_[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = params.shared_tone_curve_h_[idx];
  if (fabs(dx) <= 1e-8f) {
    return params.shared_tone_curve_ctrl_pts_y_[idx];
  }

  const float t   = (x - params.shared_tone_curve_ctrl_pts_x_[idx]) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;
  return h00 * params.shared_tone_curve_ctrl_pts_y_[idx] +
         h10 * dx * params.shared_tone_curve_m_[idx] +
         h01 * params.shared_tone_curve_ctrl_pts_y_[idx + 1] +
         h11 * dx * params.shared_tone_curve_m_[idx + 1];
}

static inline float3 metal_reconstruct_shared_tone_rgb(float3 rgb, float source_luma,
                                                       float mapped_luma) {
  const float3 delta        = rgb - float3(source_luma);

  float scale        = 1.0f;
  if (delta.x < 0.0f) scale = min(scale, mapped_luma / -delta.x);
  if (delta.y < 0.0f) scale = min(scale, mapped_luma / -delta.y);
  if (delta.z < 0.0f) scale = min(scale, mapped_luma / -delta.z);
  scale              = clamp(scale, 0.0f, 1.0f);

  return float3(mapped_luma) + delta * scale;
}

static inline float4 metal_apply_shared_tone_mapping(float4 px, constant MetalFusedParams& params) {
  if (params.shared_tone_curve_enabled_ == 0u) {
    return px;
  }

  const float source_l = metal_shared_tone_luma(px.xyz);
  float       mapped_l = metal_evaluate_shared_tone_curve(source_l, params);
  if (!isfinite(mapped_l)) {
    mapped_l = source_l;
  }

  px.xyz = metal_reconstruct_shared_tone_rgb(px.xyz, source_l, mapped_l);
  return px;
}

static inline float metal_evaluate_curve_hermite(float x, constant MetalFusedParams& params) {
  const int curve_count = params.curve_ctrl_pts_size_;
  if (curve_count <= 0) {
    return x;
  }
  if (curve_count == 1) {
    return params.curve_ctrl_pts_y_[0];
  }
  if (x <= params.curve_ctrl_pts_x_[0]) {
    return params.curve_ctrl_pts_y_[0];
  }
  if (x >= params.curve_ctrl_pts_x_[curve_count - 1]) {
    return params.curve_ctrl_pts_y_[curve_count - 1];
  }

  int idx = curve_count - 2;
  for (int i = 0; i < curve_count - 1; ++i) {
    if (x < params.curve_ctrl_pts_x_[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = params.curve_h_[idx];
  if (fabs(dx) <= 1e-8f) {
    return params.curve_ctrl_pts_y_[idx];
  }

  const float t   = (x - params.curve_ctrl_pts_x_[idx]) / dx;
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;
  const float y   = h00 * params.curve_ctrl_pts_y_[idx] + h10 * dx * params.curve_m_[idx] +
                  h01 * params.curve_ctrl_pts_y_[idx + 1] + h11 * dx * params.curve_m_[idx + 1];
  return y;
}

static inline float4 GPU_ExposureOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.exposure_enabled_ == 0u) {
    return px;
  }
  px.xyz += params.exposure_offset_;
  return px;
}

static inline float4 GPU_ToneOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.white_enabled_ == 0u && params.black_enabled_ == 0u) {
    return px;
  }
  px.xyz = px.xyz * params.slope_ + params.black_point_;
  return px;
}

static inline float4 GPU_ContrastOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.contrast_enabled_ == 0u) {
    return px;
  }
  px.xyz = metal_contrast_on_luma_acescc(px.xyz, params.contrast_scale_);
  return px;
}

static inline float4 GPU_HighlightOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.highlights_enabled_ == 0u || params.shared_tone_curve_apply_in_highlights_ == 0u) {
    return px;
  }
  return metal_apply_shared_tone_mapping(px, params);
}

static inline float4 GPU_ShadowOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.shadows_enabled_ == 0u || params.shared_tone_curve_apply_in_shadows_ == 0u) {
    return px;
  }
  return metal_apply_shared_tone_mapping(px, params);
}

static inline float4 GPU_CurveOpKernel(float4 px, constant MetalFusedParams& params) {
  if (params.curve_enabled_ == 0u || params.curve_ctrl_pts_size_ <= 0) {
    return px;
  }

  constexpr float kCurveInfluence = 0.65f;
  const float lum                 = metal_luma(px.xyz);
  const float mapped_lum          = metal_evaluate_curve_hermite(lum, params);
  const float new_lum             = lum + (mapped_lum - lum) * kCurveInfluence;
  const float ratio               = (lum > 1e-5f) ? (new_lum / lum) : 0.0f;
  px.xyz *= ratio;
  return px;
}
