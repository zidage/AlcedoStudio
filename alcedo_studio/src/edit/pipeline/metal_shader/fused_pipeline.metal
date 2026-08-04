//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "../../operators/GPU_kernels/metal_shader/basic.metal"
#include "../../operators/GPU_kernels/metal_shader/color.metal"
#include "../../operators/GPU_kernels/metal_shader/common.metal"
#include "../../operators/GPU_kernels/metal_shader/cst.metal"
#include "../../operators/GPU_kernels/metal_shader/detail.metal"

static inline float4 metal_fused_pre_hs(float4 px, constant MetalFusedParams& params) {
  px = GPU_TOWS_Kernel(px, params);
  px = GPU_ExposureOpKernel(px, params);
  px = GPU_ContrastOpKernel(px, params);
  px = GPU_ToneOpKernel(px, params);
  return px;
}

static inline float4 metal_fused_post_hs(float4 px, constant MetalFusedParams& params,
                                         device const float4* lmt_lut) {
  px = GPU_CurveOpKernel(px, params);
  px = GPU_VibranceOpKernel(px, params);
  px = GPU_ColorWheelOpKernel(px, params);
  px = GPU_HLSOpKernel(px, params);
  px = GPU_LMT_Kernel(px, params, lmt_lut);
  px = GPU_OUTPUT_Kernel(px, params);
  return px;
}

static inline float4 metal_fused_full(float4 px, constant MetalFusedParams& params,
                                      device const float4* lmt_lut) {
  px = metal_fused_pre_hs(px, params);
  px = GPU_HighlightOpKernel(px, params);
  px = GPU_ShadowOpKernel(px, params);
  px = metal_fused_post_hs(px, params, lmt_lut);
  return px;
}

kernel void metal_fused_pipeline_rgba32f(texture2d<float, access::read>  src [[texture(0)]],
                                         texture2d<float, access::write> dst [[texture(1)]],
                                         constant MetalFusedParams&      params [[buffer(0)]],
                                         device const float4*            lmt_lut [[buffer(1)]],
                                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  float4 px = src.read(gid);
  px        = metal_fused_full(px, params, lmt_lut);

  (void)lmt_lut;
  dst.write(px, gid);
}

kernel void metal_fused_stage_rgba32f(texture2d<float, access::read>  src [[texture(0)]],
                                      texture2d<float, access::write> dst [[texture(1)]],
                                      constant MetalFusedParams&      params [[buffer(0)]],
                                      device const float4*            lmt_lut [[buffer(1)]],
                                      constant int&                   stage [[buffer(2)]],
                                      uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  float4 px = src.read(gid);
  if (stage == 1) {
    px = metal_fused_pre_hs(px, params);
  } else if (stage == 2) {
    px = metal_fused_post_hs(px, params, lmt_lut);
  } else {
    px = metal_fused_full(px, params, lmt_lut);
  }

  (void)lmt_lut;
  dst.write(px, gid);
}

kernel void metal_hs_build_log_base_h_rgba32f(texture2d<float, access::read>  src [[texture(0)]],
                                              texture2d<float, access::write> dst [[texture(1)]],
                                              constant MetalFusedParams&      params [[buffer(0)]],
                                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  const int width     = static_cast<int>(dst.get_width());
  const int x         = static_cast<int>(gid.x);
  const int y         = static_cast<int>(gid.y);
  const int tap_count = params.hs_base_gaussian_tap_count_;
  if (tap_count <= 0) {
    dst.write(float4(metal_hs_log_intensity_from_acescc(src.read(gid))), gid);
    return;
  }

  const float center     = metal_hs_log_intensity_from_acescc(src.read(gid));
  float       base       = center * params.hs_base_gaussian_weights_[0];
  float       weight_sum = params.hs_base_gaussian_weights_[0];
  for (int tap = 1; tap < tap_count; ++tap) {
    const int   ax = min(x + tap, width - 1);
    const int   bx = max(x - tap, 0);
    const float wa = metal_hs_log_intensity_from_acescc(
        src.read(uint2(static_cast<uint>(ax), static_cast<uint>(y))));
    const float wb = metal_hs_log_intensity_from_acescc(
        src.read(uint2(static_cast<uint>(bx), static_cast<uint>(y))));
    const float spatial = params.hs_base_gaussian_weights_[tap];
    const float aw      = spatial * metal_hs_range_weight(center, wa);
    const float bw      = spatial * metal_hs_range_weight(center, wb);
    base += wa * aw + wb * bw;
    weight_sum += aw + bw;
  }

  dst.write(float4(base / fmax(weight_sum, 1.0e-6f)), gid);
}

kernel void metal_hs_build_log_base_v_rgba32f(texture2d<float, access::read> guidance
                                              [[texture(0)]],
                                              texture2d<float, access::read>  src [[texture(1)]],
                                              texture2d<float, access::write> dst [[texture(2)]],
                                              constant MetalFusedParams&      params [[buffer(0)]],
                                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  const int height    = static_cast<int>(dst.get_height());
  const int x         = static_cast<int>(gid.x);
  const int y         = static_cast<int>(gid.y);
  const int tap_count = params.hs_base_gaussian_tap_count_;
  if (tap_count <= 0) {
    dst.write(src.read(gid), gid);
    return;
  }

  const float center          = src.read(gid).x;
  const float center_guidance = metal_hs_log_intensity_from_acescc(guidance.read(gid));
  float       base            = center * params.hs_base_gaussian_weights_[0];
  float       weight_sum      = params.hs_base_gaussian_weights_[0];
  for (int tap = 1; tap < tap_count; ++tap) {
    const int   ay      = min(y + tap, height - 1);
    const int   by      = max(y - tap, 0);
    const uint2 acoord  = uint2(static_cast<uint>(x), static_cast<uint>(ay));
    const uint2 bcoord  = uint2(static_cast<uint>(x), static_cast<uint>(by));
    const float a       = src.read(acoord).x;
    const float b       = src.read(bcoord).x;
    const float ag      = metal_hs_log_intensity_from_acescc(guidance.read(acoord));
    const float bg      = metal_hs_log_intensity_from_acescc(guidance.read(bcoord));
    const float spatial = params.hs_base_gaussian_weights_[tap];
    const float aw      = spatial * metal_hs_range_weight(center_guidance, ag);
    const float bw      = spatial * metal_hs_range_weight(center_guidance, bg);
    base += a * aw + b * bw;
    weight_sum += aw + bw;
  }

  dst.write(float4(base / fmax(weight_sum, 1.0e-6f)), gid);
}

static inline float metal_hs_read_base_bilinear(texture2d<float, access::read> base_log, int width,
                                                int height, float x, float y) {
  const float clamped_x = clamp(x, 0.0f, static_cast<float>(width - 1));
  const float clamped_y = clamp(y, 0.0f, static_cast<float>(height - 1));
  const int   x0        = clamp(static_cast<int>(floor(clamped_x)), 0, width - 1);
  const int   y0        = clamp(static_cast<int>(floor(clamped_y)), 0, height - 1);
  const int   x1        = min(x0 + 1, width - 1);
  const int   y1        = min(y0 + 1, height - 1);
  const float tx        = clamped_x - static_cast<float>(x0);
  const float ty        = clamped_y - static_cast<float>(y0);

  const float v00       = base_log.read(uint2(static_cast<uint>(x0), static_cast<uint>(y0))).x;
  const float v10       = base_log.read(uint2(static_cast<uint>(x1), static_cast<uint>(y0))).x;
  const float v01       = base_log.read(uint2(static_cast<uint>(x0), static_cast<uint>(y1))).x;
  const float v11       = base_log.read(uint2(static_cast<uint>(x1), static_cast<uint>(y1))).x;
  const float vx0       = mix(v00, v10, tx);
  const float vx1       = mix(v01, v11, tx);
  return mix(vx0, vx1, ty);
}

struct MetalHsApplyParams {
  int base_width_;
  int base_height_;
  int use_reference_base_;
  int reserved_;
};

kernel void metal_hs_apply_local_tone_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                              texture2d<float, access::read> base_log
                                              [[texture(1)]],
                                              texture2d<float, access::write> dst [[texture(2)]],
                                              constant MetalFusedParams&      params [[buffer(0)]],
                                              constant MetalHsApplyParams&    apply_params
                                              [[buffer(1)]],
                                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  const int width       = static_cast<int>(dst.get_width());
  const int height      = static_cast<int>(dst.get_height());
  const int base_width  = max(apply_params.base_width_, 1);
  const int base_height = max(apply_params.base_height_, 1);
  const int x           = static_cast<int>(gid.x);
  const int y           = static_cast<int>(gid.y);

  float     base        = 0.0f;
  if (apply_params.use_reference_base_ != 0) {
    const float reference_width =
        static_cast<float>(max(params.render_roi_reference_width_, width));
    const float reference_height =
        static_cast<float>(max(params.render_roi_reference_height_, height));
    const float roi_origin_x =
        (params.render_roi_enabled_ != 0u) ? static_cast<float>(params.render_roi_x_) : 0.0f;
    const float roi_origin_y =
        (params.render_roi_enabled_ != 0u) ? static_cast<float>(params.render_roi_y_) : 0.0f;
    const float roi_width  = (params.render_roi_enabled_ != 0u)
                                 ? fmax(params.render_roi_scale_x_ * reference_width, 1.0f)
                                 : reference_width;
    const float roi_height = (params.render_roi_enabled_ != 0u)
                                 ? fmax(params.render_roi_scale_y_ * reference_height, 1.0f)
                                 : reference_height;
    const float reference_x =
        roi_origin_x +
        ((static_cast<float>(x) + 0.5f) * roi_width / fmax(static_cast<float>(width), 1.0f)) - 0.5f;
    const float reference_y =
        roi_origin_y +
        ((static_cast<float>(y) + 0.5f) * roi_height / fmax(static_cast<float>(height), 1.0f)) -
        0.5f;
    const float base_x =
        ((reference_x + 0.5f) * static_cast<float>(base_width) / fmax(reference_width, 1.0f)) -
        0.5f;
    const float base_y =
        ((reference_y + 0.5f) * static_cast<float>(base_height) / fmax(reference_height, 1.0f)) -
        0.5f;
    base = metal_hs_read_base_bilinear(base_log, base_width, base_height, base_x, base_y);
  } else {
    base = base_log.read(gid).x;
  }

  dst.write(GPU_HighlightShadowLocalToneOpKernel(src.read(gid), base, params), gid);
}

struct MetalHsExtractParams {
  int src_width_;
  int src_height_;
  int dst_width_;
  int dst_height_;
};

struct MetalHsRemapParams {
  int   width_;
  int   height_;
  float gamma_;
  float target_;
  float beta_;
  float alpha_;
  float sigma_r_;
  int   dst_offset_;
};

struct MetalHsRemapPackedParams {
  int   width_;
  int   height_;
  int   sample_count_;
  int   reserved_;
  float sigma_r_;
  float gammas_[32];
  float targets_[32];
  float betas_[32];
  float alphas_[32];
};

struct MetalHsPyrDownParams {
  int src_width_;
  int src_height_;
  int dst_width_;
  int dst_height_;
  int src_offset_;
  int dst_offset_;
  int reserved0_;
  int reserved1_;
};

struct MetalHsPyrDownPackedParams {
  int src_width_;
  int src_height_;
  int dst_width_;
  int dst_height_;
  int sample_count_;
  int reserved0_;
  int reserved1_;
  int reserved2_;
};

struct MetalHsSelectParams {
  int   width_;
  int   height_;
  int   coarse_width_;
  int   coarse_height_;
  float gamma_lo_;
  float gamma_hi_;
  int   first_pair_;
  int   last_pair_;
  int   top_level_;
  int   reserved0_;
  int   reserved1_;
  int   reserved2_;
};

struct MetalHsSelectPackedParams {
  int   width_;
  int   height_;
  int   coarse_width_;
  int   coarse_height_;
  int   sample_count_;
  int   top_level_;
  int   reserved0_;
  int   reserved1_;
  float gammas_[32];
};

struct MetalHsPlaneApplyParams {
  int width_;
  int height_;
  int adjusted_width_;
  int adjusted_height_;
};

static inline float metal_hs_read_plane_clamped(device const float* src, int x, int y, int width,
                                                int height) {
  const int cx = clamp(x, 0, width - 1);
  const int cy = clamp(y, 0, height - 1);
  return src[static_cast<size_t>(cy) * static_cast<size_t>(width) + static_cast<size_t>(cx)];
}

static inline float metal_hs_pyr_weight_1d(int tap) {
  if (tap == -2 || tap == 2) {
    return 1.0f / 16.0f;
  }
  if (tap == -1 || tap == 1) {
    return 4.0f / 16.0f;
  }
  return 6.0f / 16.0f;
}

static inline float metal_hs_expand_from_coarse(device const float* coarse, int coarse_width,
                                                int coarse_height, int x, int y) {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) {
      continue;
    }
    const int   cy = clamp(sample_y / 2, 0, coarse_height - 1);
    const float wy = metal_hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) {
        continue;
      }
      const int   cx = clamp(sample_x / 2, 0, coarse_width - 1);
      const float wx = metal_hs_pyr_weight_1d(kx);
      sum += 4.0f * wx * wy *
             coarse[static_cast<size_t>(cy) * static_cast<size_t>(coarse_width) +
                    static_cast<size_t>(cx)];
    }
  }
  return sum;
}

static inline float4 metal_hs_read_rgba_bilinear(texture2d<float, access::read> src, int width,
                                                 int height, float x, float y) {
  const float  clamped_x = clamp(x, 0.0f, static_cast<float>(width - 1));
  const float  clamped_y = clamp(y, 0.0f, static_cast<float>(height - 1));
  const int    x0        = clamp(static_cast<int>(floor(clamped_x)), 0, width - 1);
  const int    y0        = clamp(static_cast<int>(floor(clamped_y)), 0, height - 1);
  const int    x1        = min(x0 + 1, width - 1);
  const int    y1        = min(y0 + 1, height - 1);
  const float  tx        = clamped_x - static_cast<float>(x0);
  const float  ty        = clamped_y - static_cast<float>(y0);

  const float4 v00       = src.read(uint2(static_cast<uint>(x0), static_cast<uint>(y0)));
  const float4 v10       = src.read(uint2(static_cast<uint>(x1), static_cast<uint>(y0)));
  const float4 v01       = src.read(uint2(static_cast<uint>(x0), static_cast<uint>(y1)));
  const float4 v11       = src.read(uint2(static_cast<uint>(x1), static_cast<uint>(y1)));
  const float4 vx0       = mix(v00, v10, tx);
  const float4 vx1       = mix(v01, v11, tx);
  return mix(vx0, vx1, ty);
}

static inline float metal_hs_read_plane_bilinear(device const float* plane, int width, int height,
                                                 float x, float y) {
  const float clamped_x = clamp(x, 0.0f, static_cast<float>(width - 1));
  const float clamped_y = clamp(y, 0.0f, static_cast<float>(height - 1));
  const int   x0        = clamp(static_cast<int>(floor(clamped_x)), 0, width - 1);
  const int   y0        = clamp(static_cast<int>(floor(clamped_y)), 0, height - 1);
  const int   x1        = min(x0 + 1, width - 1);
  const int   y1        = min(y0 + 1, height - 1);
  const float tx        = clamped_x - static_cast<float>(x0);
  const float ty        = clamped_y - static_cast<float>(y0);

  const float v00 =
      plane[static_cast<size_t>(y0) * static_cast<size_t>(width) + static_cast<size_t>(x0)];
  const float v10 =
      plane[static_cast<size_t>(y0) * static_cast<size_t>(width) + static_cast<size_t>(x1)];
  const float v01 =
      plane[static_cast<size_t>(y1) * static_cast<size_t>(width) + static_cast<size_t>(x0)];
  const float v11 =
      plane[static_cast<size_t>(y1) * static_cast<size_t>(width) + static_cast<size_t>(x1)];
  const float vx0 = mix(v00, v10, tx);
  const float vx1 = mix(v01, v11, tx);
  return mix(vx0, vx1, ty);
}

kernel void metal_hs_extract_log_intensity_rgba32f(
    texture2d<float, access::read> src [[texture(0)]], device float* dst [[buffer(0)]],
    constant MetalHsExtractParams& params [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.dst_width_) ||
      gid.y >= static_cast<uint>(params.dst_height_)) {
    return;
  }

  const size_t offset = static_cast<size_t>(gid.y) * static_cast<size_t>(params.dst_width_) +
                        static_cast<size_t>(gid.x);
  dst[offset] = metal_hs_log_intensity_from_acescc(src.read(gid));
}

kernel void metal_hs_extract_log_intensity_resampled_rgba32f(
    texture2d<float, access::read> src [[texture(0)]], device float* dst [[buffer(0)]],
    constant MetalHsExtractParams& params [[buffer(1)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.dst_width_) ||
      gid.y >= static_cast<uint>(params.dst_height_)) {
    return;
  }

  const float src_x = ((static_cast<float>(gid.x) + 0.5f) * static_cast<float>(params.src_width_) /
                       fmax(static_cast<float>(params.dst_width_), 1.0f)) -
                      0.5f;
  const float src_y = ((static_cast<float>(gid.y) + 0.5f) *
                       static_cast<float>(params.src_height_) /
                       fmax(static_cast<float>(params.dst_height_), 1.0f)) -
                      0.5f;
  const size_t offset = static_cast<size_t>(gid.y) * static_cast<size_t>(params.dst_width_) +
                        static_cast<size_t>(gid.x);
  dst[offset] = metal_hs_log_intensity_from_acescc(
      metal_hs_read_rgba_bilinear(src, params.src_width_, params.src_height_, src_x, src_y));
}

kernel void metal_hs_build_remapped_sample(device const float*          source_l [[buffer(0)]],
                                           device float*                remapped_l [[buffer(1)]],
                                           constant MetalHsRemapParams& params [[buffer(2)]],
                                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const size_t dst_offset = static_cast<size_t>(params.dst_offset_) +
                            static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) +
                            static_cast<size_t>(gid.x);
  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  const float source_value = source_l[offset];
  remapped_l[dst_offset] =
      params.target_ + metal_hs_llf_remap_delta(source_value - params.gamma_, params.sigma_r_,
                                                params.alpha_, params.beta_);
}

kernel void metal_hs_build_remapped_samples_packed(device const float* source_l [[buffer(0)]],
                                                   device float*       remapped_l [[buffer(1)]],
                                                   constant MetalHsRemapPackedParams& params
                                                   [[buffer(2)]],
                                                   uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_) ||
      gid.z >= static_cast<uint>(params.sample_count_)) {
    return;
  }

  const size_t plane_elems =
      static_cast<size_t>(params.width_) * static_cast<size_t>(params.height_);
  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  const int   sample_index = static_cast<int>(gid.z);
  const float source_value = source_l[offset];
  remapped_l[static_cast<size_t>(sample_index) * plane_elems + offset] =
      params.targets_[sample_index] +
      metal_hs_llf_remap_delta(source_value - params.gammas_[sample_index], params.sigma_r_,
                               params.alphas_[sample_index], params.betas_[sample_index]);
}

kernel void metal_hs_pyr_down(device const float*            src [[buffer(0)]],
                              device float*                  dst [[buffer(1)]],
                              constant MetalHsPyrDownParams& params [[buffer(2)]],
                              uint2                          gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.dst_width_) ||
      gid.y >= static_cast<uint>(params.dst_height_)) {
    return;
  }

  const int center_x = static_cast<int>(gid.x) * 2;
  const int center_y = static_cast<int>(gid.y) * 2;
  float     sum      = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const float wy = metal_hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const float wx = metal_hs_pyr_weight_1d(kx);
      sum +=
          wx * wy *
          metal_hs_read_plane_clamped(src + static_cast<size_t>(params.src_offset_), center_x + kx,
                                      center_y + ky, params.src_width_, params.src_height_);
    }
  }

  dst[static_cast<size_t>(params.dst_offset_) +
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.dst_width_) +
      static_cast<size_t>(gid.x)] = sum;
}

kernel void metal_hs_pyr_down_packed(device const float*                  src [[buffer(0)]],
                                     device float*                        dst [[buffer(1)]],
                                     constant MetalHsPyrDownPackedParams& params [[buffer(2)]],
                                     uint3 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.dst_width_) ||
      gid.y >= static_cast<uint>(params.dst_height_) ||
      gid.z >= static_cast<uint>(params.sample_count_)) {
    return;
  }

  const size_t src_plane_elems =
      static_cast<size_t>(params.src_width_) * static_cast<size_t>(params.src_height_);
  const size_t dst_plane_elems =
      static_cast<size_t>(params.dst_width_) * static_cast<size_t>(params.dst_height_);
  const size_t src_offset = static_cast<size_t>(gid.z) * src_plane_elems;
  const size_t dst_offset = static_cast<size_t>(gid.z) * dst_plane_elems;

  const int    center_x   = static_cast<int>(gid.x) * 2;
  const int    center_y   = static_cast<int>(gid.y) * 2;
  float        sum        = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const float wy = metal_hs_pyr_weight_1d(ky);
    for (int kx = -2; kx <= 2; ++kx) {
      const float wx = metal_hs_pyr_weight_1d(kx);
      sum += wx * wy *
             metal_hs_read_plane_clamped(src + src_offset, center_x + kx, center_y + ky,
                                         params.src_width_, params.src_height_);
    }
  }

  dst[dst_offset + static_cast<size_t>(gid.y) * static_cast<size_t>(params.dst_width_) +
      static_cast<size_t>(gid.x)] = sum;
}

kernel void metal_hs_select_interpolated_level(device const float* source_level [[buffer(0)]],
                                               device const float* sample_lo_level [[buffer(1)]],
                                               device const float* sample_lo_coarse [[buffer(2)]],
                                               device const float* sample_hi_level [[buffer(3)]],
                                               device const float* sample_hi_coarse [[buffer(4)]],
                                               device float*       output_level [[buffer(5)]],
                                               constant MetalHsSelectParams& params [[buffer(6)]],
                                               uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  const float g           = source_level[offset];
  const bool  in_interval = (params.first_pair_ != 0 && g <= params.gamma_hi_) ||
                             (params.last_pair_ != 0 && g >= params.gamma_lo_) ||
                             (g >= params.gamma_lo_ && g < params.gamma_hi_);
  if (!in_interval) {
    return;
  }

  const float t = clamp((g - params.gamma_lo_) / fmax(params.gamma_hi_ - params.gamma_lo_, 1.0e-6f),
                        0.0f, 1.0f);
  if (params.top_level_ != 0) {
    output_level[offset] = mix(sample_lo_level[offset], sample_hi_level[offset], t);
    return;
  }

  const int   x = static_cast<int>(gid.x);
  const int   y = static_cast<int>(gid.y);
  const float lap_lo =
      sample_lo_level[offset] - metal_hs_expand_from_coarse(sample_lo_coarse, params.coarse_width_,
                                                            params.coarse_height_, x, y);
  const float lap_hi =
      sample_hi_level[offset] - metal_hs_expand_from_coarse(sample_hi_coarse, params.coarse_width_,
                                                            params.coarse_height_, x, y);
  output_level[offset] = mix(lap_lo, lap_hi, t);
}

kernel void metal_hs_select_interpolated_level_packed(
    device const float* source_level [[buffer(0)]], device const float* sample_level [[buffer(1)]],
    device const float* sample_coarse [[buffer(2)]], device float* output_level [[buffer(3)]],
    constant MetalHsSelectPackedParams& params [[buffer(4)]],
    uint2                               gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }
  if (params.sample_count_ < 2) {
    return;
  }

  const size_t plane_elems =
      static_cast<size_t>(params.width_) * static_cast<size_t>(params.height_);
  const size_t coarse_elems =
      static_cast<size_t>(params.coarse_width_) * static_cast<size_t>(params.coarse_height_);
  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  const float g = source_level[offset];
  if (isnan(g)) {
    output_level[offset] = 0.0f;
    return;
  }

  int pair = 0;
  if (g <= params.gammas_[1]) {
    pair = 0;
  } else if (g >= params.gammas_[params.sample_count_ - 2]) {
    pair = params.sample_count_ - 2;
  } else {
    for (int i = 1; i + 1 < params.sample_count_; ++i) {
      if (g >= params.gammas_[i] && g < params.gammas_[i + 1]) {
        pair = i;
        break;
      }
    }
  }

  const float  gamma_lo  = params.gammas_[pair];
  const float  gamma_hi  = params.gammas_[pair + 1];
  const float  t         = clamp((g - gamma_lo) / fmax(gamma_hi - gamma_lo, 1.0e-6f), 0.0f, 1.0f);
  const size_t lo_offset = static_cast<size_t>(pair) * plane_elems + offset;
  const size_t hi_offset = static_cast<size_t>(pair + 1) * plane_elems + offset;
  if (params.top_level_ != 0) {
    output_level[offset] = mix(sample_level[lo_offset], sample_level[hi_offset], t);
    return;
  }

  const int           x                = static_cast<int>(gid.x);
  const int           y                = static_cast<int>(gid.y);
  device const float* sample_lo_coarse = sample_coarse + static_cast<size_t>(pair) * coarse_elems;
  device const float* sample_hi_coarse =
      sample_coarse + static_cast<size_t>(pair + 1) * coarse_elems;
  const float lap_lo =
      sample_level[lo_offset] - metal_hs_expand_from_coarse(sample_lo_coarse, params.coarse_width_,
                                                            params.coarse_height_, x, y);
  const float lap_hi =
      sample_level[hi_offset] - metal_hs_expand_from_coarse(sample_hi_coarse, params.coarse_width_,
                                                            params.coarse_height_, x, y);
  output_level[offset] = mix(lap_lo, lap_hi, t);
}

kernel void metal_hs_collapse_level(device const float*            lap_level [[buffer(0)]],
                                    device const float*            coarse_level [[buffer(1)]],
                                    device float*                  dst_level [[buffer(2)]],
                                    constant MetalHsPyrDownParams& params [[buffer(3)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.src_width_) ||
      gid.y >= static_cast<uint>(params.src_height_)) {
    return;
  }

  const size_t offset = static_cast<size_t>(gid.y) * static_cast<size_t>(params.src_width_) +
                        static_cast<size_t>(gid.x);
  dst_level[offset] = lap_level[offset] + metal_hs_expand_from_coarse(
                                              coarse_level, params.dst_width_, params.dst_height_,
                                              static_cast<int>(gid.x), static_cast<int>(gid.y));
}

kernel void metal_hs_apply_adjusted_l_rgba32f(texture2d<float, access::read> src [[texture(0)]],
                                              device const float* adjusted_l [[buffer(0)]],
                                              texture2d<float, access::write>   dst [[texture(1)]],
                                              constant MetalHsPlaneApplyParams& params
                                              [[buffer(1)]],
                                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  dst.write(metal_hs_apply_adjusted_l_pixel(src.read(gid), adjusted_l[offset]), gid);
}

kernel void metal_hs_apply_adjusted_l_with_reference_rgba32f(
    texture2d<float, access::read> src [[texture(0)]], device const float* reference_l [[buffer(0)]],
    device const float* adjusted_l [[buffer(1)]], texture2d<float, access::write> dst [[texture(1)]],
    constant MetalHsPlaneApplyParams& params [[buffer(2)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const size_t offset =
      static_cast<size_t>(gid.y) * static_cast<size_t>(params.width_) + static_cast<size_t>(gid.x);
  dst.write(metal_hs_apply_adjusted_l_delta_pixel(src.read(gid), reference_l[offset],
                                                  adjusted_l[offset]),
            gid);
}

kernel void metal_hs_apply_adjusted_l_from_frame_rgba32f(
    texture2d<float, access::read> src [[texture(0)]], device const float* reference_l [[buffer(0)]],
    device const float* adjusted_l [[buffer(1)]], texture2d<float, access::write> dst [[texture(1)]],
    constant MetalHsPlaneApplyParams& params [[buffer(2)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const float adjusted_x =
      ((static_cast<float>(gid.x) + 0.5f) * static_cast<float>(params.adjusted_width_) /
       fmax(static_cast<float>(params.width_), 1.0f)) -
      0.5f;
  const float adjusted_y =
      ((static_cast<float>(gid.y) + 0.5f) * static_cast<float>(params.adjusted_height_) /
       fmax(static_cast<float>(params.height_), 1.0f)) -
      0.5f;
  const float sampled_reference =
      metal_hs_read_plane_bilinear(reference_l, params.adjusted_width_, params.adjusted_height_,
                                   adjusted_x, adjusted_y);
  const float sampled_adjusted =
      metal_hs_read_plane_bilinear(adjusted_l, params.adjusted_width_, params.adjusted_height_,
                                   adjusted_x, adjusted_y);
  dst.write(metal_hs_apply_adjusted_l_delta_pixel(src.read(gid), sampled_reference,
                                                  sampled_adjusted),
            gid);
}

kernel void metal_hs_apply_adjusted_l_from_reference_rgba32f(
    texture2d<float, access::read> src [[texture(0)]], device const float* reference_l [[buffer(0)]],
    device const float* adjusted_l [[buffer(1)]], texture2d<float, access::write> dst [[texture(1)]],
    constant MetalFusedParams& fused_params [[buffer(2)]],
    constant MetalHsPlaneApplyParams& params [[buffer(3)]], uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= static_cast<uint>(params.width_) || gid.y >= static_cast<uint>(params.height_)) {
    return;
  }

  const float reference_width =
      static_cast<float>(max(fused_params.render_roi_reference_width_, 1));
  const float reference_height =
      static_cast<float>(max(fused_params.render_roi_reference_height_, 1));
  const float roi_origin_x =
      (fused_params.render_roi_enabled_ != 0u) ? static_cast<float>(fused_params.render_roi_x_)
                                               : 0.0f;
  const float roi_origin_y =
      (fused_params.render_roi_enabled_ != 0u) ? static_cast<float>(fused_params.render_roi_y_)
                                               : 0.0f;
  const float roi_width = (fused_params.render_roi_enabled_ != 0u)
                              ? fmax(fused_params.render_roi_scale_x_ * reference_width, 1.0f)
                              : reference_width;
  const float roi_height = (fused_params.render_roi_enabled_ != 0u)
                               ? fmax(fused_params.render_roi_scale_y_ * reference_height, 1.0f)
                               : reference_height;
  const float reference_x =
      roi_origin_x +
      ((static_cast<float>(gid.x) + 0.5f) * roi_width / fmax(static_cast<float>(params.width_), 1.0f)) -
      0.5f;
  const float reference_y =
      roi_origin_y +
      ((static_cast<float>(gid.y) + 0.5f) * roi_height /
       fmax(static_cast<float>(params.height_), 1.0f)) -
      0.5f;
  const float adjusted_x =
      ((reference_x + 0.5f) * static_cast<float>(params.adjusted_width_) /
       fmax(reference_width, 1.0f)) -
      0.5f;
  const float adjusted_y =
      ((reference_y + 0.5f) * static_cast<float>(params.adjusted_height_) /
       fmax(reference_height, 1.0f)) -
      0.5f;

  const float sampled_reference =
      metal_hs_read_plane_bilinear(reference_l, params.adjusted_width_, params.adjusted_height_,
                                   adjusted_x, adjusted_y);
  const float sampled_adjusted =
      metal_hs_read_plane_bilinear(adjusted_l, params.adjusted_width_, params.adjusted_height_,
                                   adjusted_x, adjusted_y);
  dst.write(metal_hs_apply_adjusted_l_delta_pixel(src.read(gid), sampled_reference,
                                                  sampled_adjusted),
            gid);
}
