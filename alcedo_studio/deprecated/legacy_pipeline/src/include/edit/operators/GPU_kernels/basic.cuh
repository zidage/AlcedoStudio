//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of basic image operations

#pragma once
#include <cuda_runtime.h>

#include "edit/operators/op_kernel.hpp"
#include "param.cuh"

namespace alcedo {
namespace CUDA {
// Basic point operation kernel
struct GPU_ExposureOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) {
    if (!params.exposure_enabled_) return;
    p->x = p->x + (params.exposure_offset_);
    p->y = p->y + (params.exposure_offset_);
    p->z = p->z + (params.exposure_offset_);
  }
};

struct GPU_ToneOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.white_enabled_ && !params.black_enabled_) return;
    p->x = p->x * params.slope_ + params.black_point_;
    p->y = p->y * params.slope_ + params.black_point_;
    p->z = p->z * params.slope_ + params.black_point_;
  }
};

GPU_FUNC float sigmoid(float t) { return 1.0f / (1.0f + expf(-t)); }

GPU_FUNC float contrast_sigmoid_01(float x, float k) {
  // x is assumed in [0,1]
  const float a = sigmoid(-0.5f * k);
  const float b = sigmoid(0.5f * k);
  const float y = sigmoid(k * (x - 0.5f));
  return (y - a) / (b - a);
}

GPU_FUNC float3 contrast_on_luma_acescc(float3 rgb_acescc, float k = 6.0f, float pivot = 0.5f,
                                        float range = 0.35f, float eps = 1e-6f) {
  // AP1 luma in ACEScc domain (approx approach; "professional-looking" for gentle ops)
  const float Y =
      0.2126f * rgb_acescc.x + 0.7152f * rgb_acescc.y + 0.0722f * rgb_acescc.z;

  const float lo = pivot - range;
  const float hi = pivot + range;

  // Map Y -> t in [0,1] over [lo, hi]
  const float t  = (Y - lo) / (hi - lo);

  // Outside window: identical
  if (t <= 0.0f || t >= 1.0f) return rgb_acescc;

  const float t2    = contrast_sigmoid_01(t, k);
  const float Y2    = lo + (hi - lo) * t2;

  const float scale = Y2 / fmaxf(Y, eps);

  return {rgb_acescc.x * scale, rgb_acescc.y * scale, rgb_acescc.z * scale};
}

struct GPU_ContrastOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.contrast_enabled_) return;
    float3 acescc_color = make_float3(p->x, p->y, p->z);
    acescc_color        = contrast_on_luma_acescc(acescc_color, params.contrast_scale_);
    p->x                = acescc_color.x;;
    p->y                = acescc_color.y;
    p->z                = acescc_color.z;
  }
};

GPU_FUNC float shared_tone_luma(const float3& rgb) {
  return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

GPU_FUNC float evaluate_shared_tone_curve(float x, const GPUOperatorParams& params) {
  const int curve_count = params.shared_tone_curve_ctrl_pts_size_;
  if (curve_count <= 0) return x;
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
  if (fabsf(dx) <= 1e-8f) {
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

GPU_FUNC float3 reconstruct_shared_tone_rgb(const float3& rgb, float source_luma, float mapped_luma) {
  const float3 neutral = make_float3(source_luma, source_luma, source_luma);
  const float3 delta        = make_float3(rgb.x - neutral.x, rgb.y - neutral.y, rgb.z - neutral.z);

  float scale          = 1.0f;
  if (delta.x < 0.0f) scale = fminf(scale, mapped_luma / -delta.x);
  if (delta.y < 0.0f) scale = fminf(scale, mapped_luma / -delta.y);
  if (delta.z < 0.0f) scale = fminf(scale, mapped_luma / -delta.z);
  scale                = fminf(1.0f, fmaxf(0.0f, scale));

  return make_float3(mapped_luma + delta.x * scale, mapped_luma + delta.y * scale,
                     mapped_luma + delta.z * scale);
}

GPU_FUNC float4 apply_shared_tone_mapping(float4 px, const GPUOperatorParams& params) {
  if (!params.shared_tone_curve_enabled_) {
    return px;
  }

  const float3 rgb      = make_float3(px.x, px.y, px.z);
  const float  source_l = shared_tone_luma(rgb);
  float        mapped_l = evaluate_shared_tone_curve(source_l, params);
  if (!isfinite(mapped_l)) {
    mapped_l = source_l;
  }

  const float3 mapped_rgb = reconstruct_shared_tone_rgb(rgb, source_l, mapped_l);
  px.x                    = mapped_rgb.x;
  px.y                    = mapped_rgb.y;
  px.z                    = mapped_rgb.z;
  return px;
}

struct GPU_HighlightOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.highlights_enabled_ || !params.shared_tone_curve_apply_in_highlights_) return;
    *p = apply_shared_tone_mapping(*p, params);
  }
};

struct GPU_ShadowOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.shadows_enabled_ || !params.shared_tone_curve_apply_in_shadows_) return;
    *p = apply_shared_tone_mapping(*p, params);
  }
};

GPU_FUNC float evaluate_curve_hermite(float x, const GPUOperatorParams& params) {
  const int curve_count = params.curve_ctrl_pts_size_;
  if (curve_count <= 0) return x;
  if (curve_count == 1) {
    return params.curve_ctrl_pts_y_[0];
  }

  if (x <= params.curve_ctrl_pts_x_[0]) return params.curve_ctrl_pts_y_[0];
  if (x >= params.curve_ctrl_pts_x_[curve_count - 1]) return params.curve_ctrl_pts_y_[curve_count - 1];

  int idx = curve_count - 2;
  for (int i = 0; i < curve_count - 1; ++i) {
    if (x < params.curve_ctrl_pts_x_[i + 1]) {
      idx = i;
      break;
    }
  }

  const float dx = params.curve_h_[idx];
  if (fabsf(dx) <= 1e-8f) {
    return params.curve_ctrl_pts_y_[idx];
  }

  const float t   = (x - params.curve_ctrl_pts_x_[idx]) / dx;

  // Hermite interpolation
  const float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
  const float h10 = t * t * t - 2.0f * t * t + t;
  const float h01 = -2.0f * t * t * t + 3.0f * t * t;
  const float h11 = t * t * t - t * t;

  const float y = h00 * params.curve_ctrl_pts_y_[idx] + h10 * dx * params.curve_m_[idx] +
                  h01 * params.curve_ctrl_pts_y_[idx + 1] + h11 * dx * params.curve_m_[idx + 1];
  return y;
}

struct GPU_CurveOpKernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.curve_enabled_) return;
    if (params.curve_ctrl_pts_size_ <= 0) return;

    constexpr float kCurveInfluence = 0.65f;

    const float lum     = 0.2126f * p->x + 0.7152f * p->y + 0.0722f * p->z;
    const float mapped_lum = evaluate_curve_hermite(lum, params);
    const float new_lum    = lum + (mapped_lum - lum) * kCurveInfluence;
    const float ratio   = (lum > 1e-5f) ? new_lum / lum : 0.0f;
    p->x *= ratio;
    p->y *= ratio;
    p->z *= ratio;
  }
};
};  // namespace CUDA
};  // namespace alcedo
