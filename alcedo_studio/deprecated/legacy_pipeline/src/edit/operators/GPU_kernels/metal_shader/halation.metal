//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

static inline float3 metal_halation_decode_display_linear(
    float4 pixel, constant MetalNeighborStageParams& params) {
  return eotf(float3(fmax(pixel.x, 0.0f), fmax(pixel.y, 0.0f), fmax(pixel.z, 0.0f)), params.eotf_);
}

static inline float4 metal_halation_encode_display(float3 linear, float alpha,
                                                   constant MetalNeighborStageParams& params) {
  const float3 encoded = eotf_inv(linear, params.eotf_);
  return float4(encoded.x, encoded.y, encoded.z, alpha);
}

static inline int metal_halation_blur_radius(float sigma) {
  if (!(sigma > 0.0f)) {
    return 0;
  }
  return min(max(static_cast<int>(ceil(sigma * 3.0f)), 1), METAL_NEIGHBOR_MAX_TAP_COUNT - 1);
}

static inline float metal_halation_weight(int tap, float sigma) {
  return (tap == 0) ? 1.0f : exp(-static_cast<float>(tap) / fmax(sigma, 1.0e-6f));
}

static inline float metal_halation_weight_norm(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) {
    sum += 2.0f * metal_halation_weight(tap, sigma);
  }
  return 1.0f / fmax(sum, 1.0e-6f);
}

static inline float4 metal_halation_blur_horizontal(texture2d<float, access::read> src, uint2 gid,
                                                    constant MetalNeighborStageParams& params) {
  const float  sigma         = params.sigma_x_;
  const int    radius        = metal_halation_blur_radius(sigma);
  const float  norm          = metal_halation_weight_norm(radius, sigma);
  const int2   center        = int2(static_cast<int>(gid.x), static_cast<int>(gid.y));

  const float4 center_pixel  = metal_detail_read_clamped(src, center);
  const float3 center_linear = metal_halation_decode_display_linear(center_pixel, params);
  float4       blur = float4(center_linear.x * norm, center_linear.y * norm, center_linear.z * norm,
                             center_pixel.w);

  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight       = metal_halation_weight(tap, sigma) * norm;
    const float4 left         = metal_detail_read_clamped(src, center + int2(-tap, 0));
    const float4 right        = metal_detail_read_clamped(src, center + int2(tap, 0));
    const float3 left_linear  = metal_halation_decode_display_linear(left, params);
    const float3 right_linear = metal_halation_decode_display_linear(right, params);
    blur.x += (left_linear.x + right_linear.x) * weight;
    blur.y += (left_linear.y + right_linear.y) * weight;
    blur.z += (left_linear.z + right_linear.z) * weight;
  }

  return blur;
}

static inline float4 metal_halation_blur_vertical(texture2d<float, access::read> src, uint2 gid,
                                                  constant MetalNeighborStageParams& params) {
  const float sigma  = params.sigma_y_;
  const int   radius = metal_halation_blur_radius(sigma);
  const float norm   = metal_halation_weight_norm(radius, sigma);
  const int2  center = int2(static_cast<int>(gid.x), static_cast<int>(gid.y));
  float4      blur   = metal_detail_read_clamped(src, center);
  blur.x *= norm;
  blur.y *= norm;
  blur.z *= norm;

  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight = metal_halation_weight(tap, sigma) * norm;
    const float4 top    = metal_detail_read_clamped(src, center + int2(0, -tap));
    const float4 bottom = metal_detail_read_clamped(src, center + int2(0, tap));
    blur.x += (top.x + bottom.x) * weight;
    blur.y += (top.y + bottom.y) * weight;
    blur.z += (top.z + bottom.z) * weight;
  }

  return blur;
}

static inline float4 metal_apply_halation(float4 px, float4 blur,
                                          constant MetalNeighborStageParams& params) {
  if (params.enabled_ == 0u || !(params.amount_ > 0.0f)) {
    return px;
  }

  const float3 original_linear = metal_halation_decode_display_linear(px, params);
  const float3 spill_linear =
      float3(fmax(blur.x - original_linear.x, 0.0f), fmax(blur.y - original_linear.y, 0.0f),
             fmax(blur.z - original_linear.z, 0.0f));
  const float3 result_linear =
      original_linear + spill_linear * float3(params.amount_ * params.redshift_[0],
                                              params.amount_ * params.redshift_[1],
                                              params.amount_ * params.redshift_[2]);
  return metal_halation_encode_display(result_linear, px.w, params);
}
