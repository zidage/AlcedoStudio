//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

static inline float4 opencl_fused_pre_hs(float4 px, __global const OpenClFusedParams* params) {
  px = opencl_tows_op(px, params);
  px = opencl_exposure_op(px, params);
  px = opencl_contrast_op(px, params);
  px = opencl_tone_op(px, params);
  return px;
}

static inline float4 opencl_fused_post_hs(float4 px,
                                          __global const OpenClFusedParams* params,
                                          __global const float* lmt_lut) {
  px = opencl_curve_op(px, params);
  px = opencl_vibrance_op(px, params);
  px = opencl_color_wheel_op(px, params);
  px = opencl_hls_op(px, params);
  px = opencl_lmt_op(px, params, lmt_lut);
  px = opencl_output_op(px, params);
  return px;
}

static inline float4 opencl_fused_full(float4 px,
                                       __global const OpenClFusedParams* params,
                                       __global const float* lmt_lut) {
  px = opencl_fused_pre_hs(px, params);
  px = opencl_highlight_op(px, params);
  px = opencl_shadow_op(px, params);
  px = opencl_fused_post_hs(px, params, lmt_lut);
  return px;
}

__kernel void edit_pipeline_fused_rgba32f(__global const float* input,
                                          __global float* output,
                                          __global const OpenClFusedParams* params,
                                          __global const float* lmt_lut,
                                          int width,
                                          int height) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  int idx = (y * width + x) * 4;
  float4 px = (float4)(input[idx + 0], input[idx + 1], input[idx + 2], input[idx + 3]);

  px = opencl_fused_full(px, params, lmt_lut);

  output[idx + 0] = px.x;
  output[idx + 1] = px.y;
  output[idx + 2] = px.z;
  output[idx + 3] = px.w;
}

__kernel void edit_pipeline_fused_stage_rgba32f(__global const float* input,
                                                __global float* output,
                                                __global const OpenClFusedParams* params,
                                                __global const float* lmt_lut,
                                                int width,
                                                int height,
                                                int stage) {
  int x = get_global_id(0);
  int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  int idx = (y * width + x) * 4;
  float4 px = (float4)(input[idx + 0], input[idx + 1], input[idx + 2], input[idx + 3]);

  if (stage == 1) {
    px = opencl_fused_pre_hs(px, params);
  } else if (stage == 2) {
    px = opencl_fused_post_hs(px, params, lmt_lut);
  } else {
    px = opencl_fused_full(px, params, lmt_lut);
  }

  output[idx + 0] = px.x;
  output[idx + 1] = px.y;
  output[idx + 2] = px.z;
  output[idx + 3] = px.w;
}
