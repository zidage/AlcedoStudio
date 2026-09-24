//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "cst.metal"

#define METAL_NEIGHBOR_MAX_TAP_COUNT 64

constant uint kMetalNeighborOpSharpen   = 1u;
constant uint kMetalNeighborOpClarity   = 2u;
constant uint kMetalNeighborOpHalation  = 3u;
constant uint kMetalNeighborOpFilmGrain = 4u;

struct MetalNeighborStageParams {
  uint  kind_;
  uint  radius_;
  uint  tap_count_;
  float amount_;
  float threshold_;
  float weights_[METAL_NEIGHBOR_MAX_TAP_COUNT];
  uint  enabled_;
  int   eotf_;
  uint  seed_lo_;
  uint  seed_hi_;
  float sigma_x_;
  float sigma_y_;
  float redshift_[3];
  float reserved_;
  uint  roi_enabled_;
  int   roi_x_;
  int   roi_y_;
  float roi_scale_x_;
  float roi_scale_y_;
  int   roi_reference_width_;
  int   roi_reference_height_;
  uint  reserved_tail_;
};

static inline float metal_detail_luminance(float4 c) {
  // Match the CUDA implementation's COLOR_BGR2GRAY coefficients.
  return c.x * 0.114f + c.y * 0.587f + c.z * 0.299f;
}

static inline float4 metal_detail_read_clamped(texture2d<float, access::read> src, int2 coord) {
  const int x = clamp(coord.x, 0, static_cast<int>(src.get_width()) - 1);
  const int y = clamp(coord.y, 0, static_cast<int>(src.get_height()) - 1);
  return src.read(uint2(static_cast<uint>(x), static_cast<uint>(y)));
}

static inline float metal_detail_smoothstep(float edge0, float edge1, float x) {
  const float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

#include "film_grain.metal"
#include "halation.metal"

static inline float4 metal_neighbor_blur_horizontal(texture2d<float, access::read> src, uint2 gid,
                                                    constant MetalNeighborStageParams& params) {
  if (params.enabled_ == 0u || params.amount_ == 0.0f) {
    return src.read(gid);
  }
  if (params.kind_ == kMetalNeighborOpHalation) {
    return metal_halation_blur_horizontal(src, gid, params);
  }
  if (params.kind_ == kMetalNeighborOpFilmGrain) {
    return metal_film_grain_blur_horizontal(src, gid, params);
  }
  if (params.tap_count_ == 0u) {
    return src.read(gid);
  }

  const int2 center = int2(static_cast<int>(gid.x), static_cast<int>(gid.y));
  float4     blur   = metal_detail_read_clamped(src, center) * params.weights_[0];
  for (uint tap = 1u; tap < params.tap_count_; ++tap) {
    const int    dx = static_cast<int>(tap);
    const float  w  = params.weights_[tap];
    const float4 a  = metal_detail_read_clamped(src, center + int2(dx, 0));
    const float4 b  = metal_detail_read_clamped(src, center - int2(dx, 0));
    blur += (a + b) * w;
  }
  return blur;
}

static inline float4 metal_neighbor_blur_vertical(texture2d<float, access::read> src, uint2 gid,
                                                  constant MetalNeighborStageParams& params) {
  if (params.kind_ == kMetalNeighborOpHalation) {
    return metal_halation_blur_vertical(src, gid, params);
  }
  if (params.kind_ == kMetalNeighborOpFilmGrain) {
    return metal_film_grain_blur_vertical(src, gid);
  }
  if (params.tap_count_ == 0u) {
    return src.read(gid);
  }

  const int2 center = int2(static_cast<int>(gid.x), static_cast<int>(gid.y));
  float4     blur   = metal_detail_read_clamped(src, center) * params.weights_[0];
  for (uint tap = 1u; tap < params.tap_count_; ++tap) {
    const int    dy = static_cast<int>(tap);
    const float  w  = params.weights_[tap];
    const float4 a  = metal_detail_read_clamped(src, center + int2(0, dy));
    const float4 b  = metal_detail_read_clamped(src, center - int2(0, dy));
    blur += (a + b) * w;
  }
  return blur;
}

static inline float4 metal_apply_sharpen(float4 px, float4 blur,
                                         constant MetalNeighborStageParams& params) {
  if (params.amount_ == 0.0f || params.tap_count_ == 0u) {
    return px;
  }

  float4 high = px - blur;

  if (params.threshold_ > 0.0f) {
    const float hp_gray = metal_detail_luminance(high);
    const float mask    = (fabs(hp_gray) > params.threshold_) ? 1.0f : 0.0f;
    high *= mask;
  }

  return px + high * params.amount_;
}

static inline float4 metal_apply_clarity(float4 px, float4 blur,
                                         constant MetalNeighborStageParams& params) {
  if (params.amount_ == 0.0f || params.tap_count_ == 0u) {
    return px;
  }

  float4          diff           = float4(px.x - blur.x, px.y - blur.y, px.z - blur.z, 0.0f);

  const float     diff_lum       = metal_detail_luminance(diff);
  const float     edge_mag       = fabs(diff_lum);
  constexpr float kEdgeThreshold = 0.18f;
  const float     protect        = 1.0f - metal_detail_smoothstep(0.0f, kEdgeThreshold, edge_mag);

  const float     lum            = metal_detail_luminance(px);
  const float     t_lum          = (lum - 0.5f) * 2.0f;
  const float     mask           = fmax(1.0f - t_lum * t_lum, 0.0f);
  const float     strength       = params.amount_ * protect * mask;

  return float4(fma(diff.x, strength, px.x), fma(diff.y, strength, px.y),
                fma(diff.z, strength, px.z), px.w);
}

kernel void metal_neighbor_blur_h_rgba32f(texture2d<float, access::read>     src [[texture(0)]],
                                          texture2d<float, access::write>    dst [[texture(1)]],
                                          constant MetalNeighborStageParams& params [[buffer(0)]],
                                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  dst.write(metal_neighbor_blur_horizontal(src, gid, params), gid);
}

kernel void metal_neighbor_apply_v_rgba32f(texture2d<float, access::read>     src [[texture(0)]],
                                           texture2d<float, access::read>     blur_h [[texture(1)]],
                                           texture2d<float, access::write>    dst [[texture(2)]],
                                           constant MetalNeighborStageParams& params [[buffer(0)]],
                                           uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }

  const float4 px   = src.read(gid);
  const float4 blur = metal_neighbor_blur_vertical(blur_h, gid, params);

  switch (params.kind_) {
    case kMetalNeighborOpSharpen:
      dst.write(metal_apply_sharpen(px, blur, params), gid);
      break;
    case kMetalNeighborOpClarity:
      dst.write(metal_apply_clarity(px, blur, params), gid);
      break;
    case kMetalNeighborOpHalation:
      dst.write(metal_apply_halation(px, blur, params), gid);
      break;
    case kMetalNeighborOpFilmGrain:
      dst.write(metal_apply_film_grain(px, blur, params), gid);
      break;
    default:
      dst.write(px, gid);
      break;
  }
}
