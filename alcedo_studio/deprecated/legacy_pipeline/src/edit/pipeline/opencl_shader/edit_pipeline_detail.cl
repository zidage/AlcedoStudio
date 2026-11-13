//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_CL

// === Separable blur helpers =====================================================

static inline float4 opencl_neighbor_blur_horizontal(__global const float4* src, int x, int y,
                                                     int width, int height,
                                                     __global const OpenClNeighborStageParams* params) {
  if (params->enabled_ == 0u || params->amount_ == 0.0f) {
    return opencl_detail_read_clamped(src, x, y, width, height);
  }
  if (params->kind_ == ALCEDO_OPENCL_NEIGHBOR_OP_HALATION) {
    return opencl_halation_blur_horizontal(src, x, y, width, height, params);
  }
  if (params->kind_ == ALCEDO_OPENCL_NEIGHBOR_OP_FILM_GRAIN) {
    return opencl_film_grain_blur_horizontal(src, x, y, width, height, params);
  }
  if (params->tap_count_ == 0u) {
    return opencl_detail_read_clamped(src, x, y, width, height);
  }

  float4 blur = opencl_detail_read_clamped(src, x, y, width, height) * params->weights_[0];
  for (uint tap = 1u; tap < params->tap_count_; ++tap) {
    const float  w = params->weights_[tap];
    const float4 a = opencl_detail_read_clamped(src, x + (int)tap, y, width, height);
    const float4 b = opencl_detail_read_clamped(src, x - (int)tap, y, width, height);
    blur += (a + b) * w;
  }
  return blur;
}

static inline float4 opencl_neighbor_blur_vertical(__global const float4* src, int x, int y,
                                                   int width, int height,
                                                   __global const OpenClNeighborStageParams* params) {
  if (params->kind_ == ALCEDO_OPENCL_NEIGHBOR_OP_HALATION) {
    return opencl_halation_blur_vertical(src, x, y, width, height, params);
  }
  if (params->kind_ == ALCEDO_OPENCL_NEIGHBOR_OP_FILM_GRAIN) {
    return opencl_film_grain_blur_vertical(src, x, y, width, height);
  }
  if (params->tap_count_ == 0u) {
    return opencl_detail_read_clamped(src, x, y, width, height);
  }

  float4 blur = opencl_detail_read_clamped(src, x, y, width, height) * params->weights_[0];
  for (uint tap = 1u; tap < params->tap_count_; ++tap) {
    const float  w = params->weights_[tap];
    const float4 a = opencl_detail_read_clamped(src, x, y + (int)tap, width, height);
    const float4 b = opencl_detail_read_clamped(src, x, y - (int)tap, width, height);
    blur += (a + b) * w;
  }
  return blur;
}

// === Apply operators ============================================================

static inline float4 opencl_apply_sharpen(float4 px, float4 blur,
                                          __global const OpenClNeighborStageParams* params) {
  if (params->amount_ == 0.0f || params->tap_count_ == 0u) {
    return px;
  }

  float4 high = px - blur;

  if (params->threshold_ > 0.0f) {
    const float hp_gray = opencl_detail_luminance(high);
    const float mask    = (fabs(hp_gray) > params->threshold_) ? 1.0f : 0.0f;
    high *= mask;
  }

  return px + high * params->amount_;
}

static inline float4 opencl_apply_clarity(float4 px, float4 blur,
                                          __global const OpenClNeighborStageParams* params) {
  if (params->amount_ == 0.0f || params->tap_count_ == 0u) {
    return px;
  }

  float4 diff = (float4)(px.x - blur.x, px.y - blur.y, px.z - blur.z, 0.0f);

  const float diff_lum = opencl_detail_luminance(diff);
  const float edge_mag = fabs(diff_lum);
  const float kEdgeThreshold = 0.18f;
  const float protect = 1.0f - opencl_detail_smoothstep(0.0f, kEdgeThreshold, edge_mag);

  const float lum   = opencl_detail_luminance(px);
  const float t_lum = (lum - 0.5f) * 2.0f;
  const float mask  = fmax(1.0f - t_lum * t_lum, 0.0f);
  const float strength = params->amount_ * protect * mask;

  return (float4)(fma(diff.x, strength, px.x), fma(diff.y, strength, px.y),
                  fma(diff.z, strength, px.z), px.w);
}

// === Kernels ====================================================================

__kernel void edit_pipeline_neighbor_blur_h_rgba32f(__global const float4* src,
                                                    __global float4* dst,
                                                    __global const OpenClNeighborStageParams* params,
                                                    int width,
                                                    int height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const int idx = y * width + x;
  dst[idx] = opencl_neighbor_blur_horizontal(src, x, y, width, height, params);
}

__kernel void edit_pipeline_neighbor_apply_v_rgba32f(__global const float4* src,
                                                     __global const float4* blur_h,
                                                     __global float4* dst,
                                                     __global const OpenClNeighborStageParams* params,
                                                     int width,
                                                     int height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const int   idx  = y * width + x;
  const float4 px   = src[idx];
  const float4 blur = opencl_neighbor_blur_vertical(blur_h, x, y, width, height, params);

  switch (params->kind_) {
    case ALCEDO_OPENCL_NEIGHBOR_OP_SHARPEN:
      dst[idx] = opencl_apply_sharpen(px, blur, params);
      break;
    case ALCEDO_OPENCL_NEIGHBOR_OP_CLARITY:
      dst[idx] = opencl_apply_clarity(px, blur, params);
      break;
    case ALCEDO_OPENCL_NEIGHBOR_OP_HALATION:
      dst[idx] = opencl_apply_halation(px, blur, params);
      break;
    case ALCEDO_OPENCL_NEIGHBOR_OP_FILM_GRAIN:
      dst[idx] = opencl_apply_film_grain(px, blur, params);
      break;
    default:
      dst[idx] = px;
      break;
  }
}

static inline float opencl_hs_read_plane_clamped(__global const float* src, int x, int y,
                                                 int width, int height) {
  const int cx = clamp(x, 0, width - 1);
  const int cy = clamp(y, 0, height - 1);
  return src[(size_t)cy * (size_t)width + (size_t)cx];
}

static inline float opencl_hs_pyr_weight_1d(int tap) {
  if (tap == -2 || tap == 2) {
    return 1.0f / 16.0f;
  }
  if (tap == -1 || tap == 1) {
    return 4.0f / 16.0f;
  }
  return 6.0f / 16.0f;
}

static inline float opencl_hs_expand_from_coarse(__global const float* coarse,
                                                 int coarse_width,
                                                 int coarse_height,
                                                 int x,
                                                 int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) continue;
    const int cy = clamp(sample_y / 2, 0, coarse_height - 1);
    const float wy = opencl_hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) continue;
      const int cx = clamp(sample_x / 2, 0, coarse_width - 1);
      const float wx = opencl_hs_pyr_weight_1d(kx);
      sum += 4.0f * wx * wy * coarse[(size_t)cy * (size_t)coarse_width + (size_t)cx];
    }
  }
  return sum;
}

static inline float4 opencl_hs_read_rgba_bilinear(__global const float4* src,
                                                  int width,
                                                  int height,
                                                  float x,
                                                  float y) {
  const float clamped_x = clamp(x, 0.0f, (float)(width - 1));
  const float clamped_y = clamp(y, 0.0f, (float)(height - 1));
  const int x0 = clamp((int)floor(clamped_x), 0, width - 1);
  const int y0 = clamp((int)floor(clamped_y), 0, height - 1);
  const int x1 = min(x0 + 1, width - 1);
  const int y1 = min(y0 + 1, height - 1);
  const float tx = clamped_x - (float)x0;
  const float ty = clamped_y - (float)y0;
  const float4 v00 = src[(size_t)y0 * (size_t)width + (size_t)x0];
  const float4 v10 = src[(size_t)y0 * (size_t)width + (size_t)x1];
  const float4 v01 = src[(size_t)y1 * (size_t)width + (size_t)x0];
  const float4 v11 = src[(size_t)y1 * (size_t)width + (size_t)x1];
  const float4 vx0 = v00 + (v10 - v00) * tx;
  const float4 vx1 = v01 + (v11 - v01) * tx;
  return vx0 + (vx1 - vx0) * ty;
}

static inline float opencl_hs_read_plane_bilinear(__global const float* plane,
                                                  int width,
                                                  int height,
                                                  float x,
                                                  float y) {
  const float clamped_x = clamp(x, 0.0f, (float)(width - 1));
  const float clamped_y = clamp(y, 0.0f, (float)(height - 1));
  const int x0 = clamp((int)floor(clamped_x), 0, width - 1);
  const int y0 = clamp((int)floor(clamped_y), 0, height - 1);
  const int x1 = min(x0 + 1, width - 1);
  const int y1 = min(y0 + 1, height - 1);
  const float tx = clamped_x - (float)x0;
  const float ty = clamped_y - (float)y0;
  const float v00 = plane[(size_t)y0 * (size_t)width + (size_t)x0];
  const float v10 = plane[(size_t)y0 * (size_t)width + (size_t)x1];
  const float v01 = plane[(size_t)y1 * (size_t)width + (size_t)x0];
  const float v11 = plane[(size_t)y1 * (size_t)width + (size_t)x1];
  const float vx0 = v00 + (v10 - v00) * tx;
  const float vx1 = v01 + (v11 - v01) * tx;
  return vx0 + (vx1 - vx0) * ty;
}

__kernel void edit_pipeline_hs_extract_log_intensity_rgba32f(
    __global const float4* src,
    __global float* dst,
    int width,
    int height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const int idx = y * width + x;
  dst[idx] = opencl_hs_log_intensity_from_acescc(src[idx]);
}

__kernel void edit_pipeline_hs_extract_log_intensity_resampled_rgba32f(
    __global const float4* src,
    __global float* dst,
    int src_width,
    int src_height,
    int dst_width,
    int dst_height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= dst_width || y >= dst_height) {
    return;
  }

  const float src_x =
      (((float)x + 0.5f) * (float)src_width / fmax((float)dst_width, 1.0f)) - 0.5f;
  const float src_y =
      (((float)y + 0.5f) * (float)src_height / fmax((float)dst_height, 1.0f)) - 0.5f;
  dst[(size_t)y * (size_t)dst_width + (size_t)x] =
      opencl_hs_log_intensity_from_acescc(
          opencl_hs_read_rgba_bilinear(src, src_width, src_height, src_x, src_y));
}

__kernel void edit_pipeline_hs_build_remapped_sample(
    __global const float* source_l,
    __global float* remapped_l,
    int width,
    int height,
    float gamma,
    float target,
    float beta,
    float alpha,
    float sigma_r) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }
  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  const float source_value = source_l[offset];
  remapped_l[offset] = target + opencl_hs_llf_remap_delta(source_value - gamma, sigma_r,
                                                          alpha, beta);
}

__kernel void edit_pipeline_hs_pyr_down(
    __global const float* src,
    __global float* dst,
    int src_width,
    int src_height,
    int dst_width,
    int dst_height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= dst_width || y >= dst_height) {
    return;
  }

  const int center_x = x * 2;
  const int center_y = y * 2;
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const float wy = opencl_hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const float wx = opencl_hs_pyr_weight_1d(kx);
      sum += wx * wy *
             opencl_hs_read_plane_clamped(src, center_x + kx, center_y + ky,
                                          src_width, src_height);
    }
  }
  dst[(size_t)y * (size_t)dst_width + (size_t)x] = sum;
}

__kernel void edit_pipeline_hs_select_interpolated_level(
    __global const float* source_level,
    __global const float* sample_lo_level,
    __global const float* sample_lo_coarse,
    __global const float* sample_hi_level,
    __global const float* sample_hi_coarse,
    __global float* output_level,
    int width,
    int height,
    int coarse_width,
    int coarse_height,
    float gamma_lo,
    float gamma_hi,
    int first_pair,
    int last_pair,
    int top_level) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  const float g = source_level[offset];
  const bool in_interval =
      ((first_pair != 0) && g <= gamma_hi) || ((last_pair != 0) && g >= gamma_lo) ||
      (g >= gamma_lo && g < gamma_hi);
  if (!in_interval) {
    return;
  }

  const float t = clamp((g - gamma_lo) / fmax(gamma_hi - gamma_lo, 1.0e-6f), 0.0f, 1.0f);
  if (top_level != 0) {
    output_level[offset] = sample_lo_level[offset] + (sample_hi_level[offset] -
                                                      sample_lo_level[offset]) * t;
    return;
  }

  const float lap_lo = sample_lo_level[offset] -
                       opencl_hs_expand_from_coarse(sample_lo_coarse, coarse_width,
                                                    coarse_height, x, y);
  const float lap_hi = sample_hi_level[offset] -
                       opencl_hs_expand_from_coarse(sample_hi_coarse, coarse_width,
                                                    coarse_height, x, y);
  output_level[offset] = lap_lo + (lap_hi - lap_lo) * t;
}

__kernel void edit_pipeline_hs_collapse_level(
    __global const float* lap_level,
    __global const float* coarse_level,
    __global float* dst_level,
    int width,
    int height,
    int coarse_width,
    int coarse_height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  dst_level[offset] = lap_level[offset] +
                      opencl_hs_expand_from_coarse(coarse_level, coarse_width,
                                                   coarse_height, x, y);
}

__kernel void edit_pipeline_hs_apply_adjusted_l_rgba32f(
    __global const float4* src,
    __global const float* adjusted_l,
    __global float4* dst,
    int width,
    int height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  dst[offset] = opencl_hs_apply_adjusted_l_pixel(src[offset], adjusted_l[offset]);
}

__kernel void edit_pipeline_hs_apply_adjusted_l_from_frame_rgba32f(
    __global const float4* src,
    __global const float* reference_l,
    __global const float* adjusted_l,
    __global float4* dst,
    int width,
    int height,
    int adjusted_width,
    int adjusted_height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const float adjusted_x =
      (((float)x + 0.5f) * (float)adjusted_width / fmax((float)width, 1.0f)) - 0.5f;
  const float adjusted_y =
      (((float)y + 0.5f) * (float)adjusted_height / fmax((float)height, 1.0f)) - 0.5f;
  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  const float sampled_reference =
      opencl_hs_read_plane_bilinear(reference_l, adjusted_width, adjusted_height,
                                    adjusted_x, adjusted_y);
  const float sampled_adjusted =
      opencl_hs_read_plane_bilinear(adjusted_l, adjusted_width, adjusted_height,
                                    adjusted_x, adjusted_y);
  dst[offset] =
      opencl_hs_apply_adjusted_l_delta_pixel(src[offset], sampled_reference, sampled_adjusted);
}

__kernel void edit_pipeline_hs_apply_adjusted_l_from_reference_rgba32f(
    __global const float4* src,
    __global const float* reference_l,
    __global const float* adjusted_l,
    __global float4* dst,
    __global const OpenClFusedParams* params,
    int width,
    int height,
    int adjusted_width,
    int adjusted_height) {
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }

  const float reference_width = (float)max(params->render_roi_reference_width_, 1);
  const float reference_height = (float)max(params->render_roi_reference_height_, 1);
  const float roi_origin_x =
      (params->render_roi_enabled_ != 0u) ? (float)params->render_roi_x_ : 0.0f;
  const float roi_origin_y =
      (params->render_roi_enabled_ != 0u) ? (float)params->render_roi_y_ : 0.0f;
  const float roi_width = (params->render_roi_enabled_ != 0u)
                              ? fmax(params->render_roi_scale_x_ * reference_width, 1.0f)
                              : reference_width;
  const float roi_height = (params->render_roi_enabled_ != 0u)
                               ? fmax(params->render_roi_scale_y_ * reference_height, 1.0f)
                               : reference_height;
  const float reference_x =
      roi_origin_x + (((float)x + 0.5f) * roi_width / fmax((float)width, 1.0f)) - 0.5f;
  const float reference_y =
      roi_origin_y + (((float)y + 0.5f) * roi_height / fmax((float)height, 1.0f)) - 0.5f;
  const float adjusted_x =
      ((reference_x + 0.5f) * (float)adjusted_width / fmax(reference_width, 1.0f)) - 0.5f;
  const float adjusted_y =
      ((reference_y + 0.5f) * (float)adjusted_height / fmax(reference_height, 1.0f)) - 0.5f;

  const size_t offset = (size_t)y * (size_t)width + (size_t)x;
  const float sampled_reference =
      opencl_hs_read_plane_bilinear(reference_l, adjusted_width, adjusted_height,
                                    adjusted_x, adjusted_y);
  const float sampled_adjusted =
      opencl_hs_read_plane_bilinear(adjusted_l, adjusted_width, adjusted_height,
                                    adjusted_x, adjusted_y);
  dst[offset] =
      opencl_hs_apply_adjusted_l_delta_pixel(src[offset], sampled_reference, sampled_adjusted);
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_DETAIL_CL
