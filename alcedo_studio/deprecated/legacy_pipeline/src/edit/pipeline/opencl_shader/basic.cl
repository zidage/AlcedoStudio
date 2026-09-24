//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_BASIC_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_BASIC_CL

// === TOWS: To Working Space ===================================================
// ACES2065-1 (AP0, linear) → AP1 (RGCed) → ACEScc (AP1, log encoded)

static inline float4 opencl_tows_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->to_ws_enabled_ == 0u) return px;

  float3 ap1;
  const uint use_camera_to_ap1 =
      (params->raw_decode_input_space_ == 1) && (params->color_temp_matrices_valid_ != 0u);
  if (use_camera_to_ap1) {
    ap1 = opencl_apply_matrix3x3(params->color_temp_cam_to_ap1_, px.xyz);
  } else {
    // ACES2065-1 (AP0) → ACEScg (AP1) matrix, inlined for OpenCL 1.2 compat.
    const float m[9] = {
        1.4514393161f,  -0.2365107469f, -0.2149285693f,
       -0.0765537734f,   1.1762296998f, -0.0996759264f,
        0.0083161484f,  -0.0060324498f,  0.9977163014f
    };
    ap1.x = m[0] * px.x + m[1] * px.y + m[2] * px.z;
    ap1.y = m[3] * px.x + m[4] * px.y + m[5] * px.z;
    ap1.z = m[6] * px.x + m[7] * px.y + m[8] * px.z;
  }

  const float ach     = fmax(ap1.x, fmax(ap1.y, ap1.z));
  const float abs_ach = fabs(ach);
  if (abs_ach > 1e-6f) {
    const float dist_cyan    = (ach - ap1.x) / abs_ach;
    const float dist_magenta = (ach - ap1.y) / abs_ach;
    const float dist_yellow  = (ach - ap1.z) / abs_ach;
    ap1.x = ach - opencl_rgc_compress_curve(dist_cyan, ALCEDO_OPENCL_RGC_LIM_CYAN, ALCEDO_OPENCL_RGC_THR_CYAN, ALCEDO_OPENCL_RGC_PWR) * abs_ach;
    ap1.y = ach - opencl_rgc_compress_curve(dist_magenta, ALCEDO_OPENCL_RGC_LIM_MAGENTA, ALCEDO_OPENCL_RGC_THR_MAGENTA, ALCEDO_OPENCL_RGC_PWR) * abs_ach;
    ap1.z = ach - opencl_rgc_compress_curve(dist_yellow, ALCEDO_OPENCL_RGC_LIM_YELLOW, ALCEDO_OPENCL_RGC_THR_YELLOW, ALCEDO_OPENCL_RGC_PWR) * abs_ach;
  }

  return (float4)(opencl_acescc_encode(ap1.x), opencl_acescc_encode(ap1.y), opencl_acescc_encode(ap1.z), px.w);
}

// === Exposure =================================================================

static inline float4 opencl_exposure_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->exposure_enabled_ == 0u) return px;
  px.xyz += params->exposure_offset_;
  return px;
}

// === Contrast =================================================================

static inline float4 opencl_contrast_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->contrast_enabled_ == 0u) return px;
  px.xyz = opencl_contrast_on_luma_acescc(px.xyz, params->contrast_scale_, 0.5f, 0.35f);
  return px;
}

// === Tone (White/Black point) =================================================

static inline float4 opencl_tone_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->white_enabled_ == 0u && params->black_enabled_ == 0u) return px;
  px.xyz = px.xyz * params->slope_ + params->black_point_;
  return px;
}

// === Highlights (shared tone mapping applied in highlights) ===================

static inline float4 opencl_highlight_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->highlights_enabled_ == 0u || params->shared_tone_curve_apply_in_highlights_ == 0u) return px;
  return opencl_apply_shared_tone_mapping(px, params);
}

// === Shadows (shared tone mapping applied in shadows) =========================

static inline float4 opencl_shadow_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->shadows_enabled_ == 0u || params->shared_tone_curve_apply_in_shadows_ == 0u) return px;
  return opencl_apply_shared_tone_mapping(px, params);
}

// === Curve ====================================================================

static inline float4 opencl_curve_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->curve_enabled_ == 0u || params->curve_ctrl_pts_size_ <= 0) return px;

  const float lum        = opencl_luma(px.xyz);
  const float mapped_lum = opencl_evaluate_curve_hermite(lum, params);
  const float new_lum    = lum + (mapped_lum - lum) * 0.65f;
  const float ratio      = (lum > 1e-5f) ? (new_lum / lum) : 0.0f;
  px.xyz *= ratio;
  return px;
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_BASIC_CL
