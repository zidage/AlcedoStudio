//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL
#define ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL

#define ALCEDO_OPENCL_HLS_PROFILE_COUNT 8
// === Tint =====================================================================

static inline float4 opencl_tint_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->tint_enabled_ == 0u) return px;
  px.y += params->tint_offset_;
  return px;
}

// === Vibrance =================================================================

static inline float4 opencl_vibrance_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->vibrance_enabled_ == 0u) return px;

  const float max_val  = fmax(fmax(px.x, px.y), px.z);
  const float min_val  = fmin(fmin(px.x, px.y), px.z);
  const float chroma   = max_val - min_val;
  const float strength = params->vibrance_offset_;
  const float falloff  = exp(-3.0f * chroma);
  const float scale    = 1.0f + strength * falloff;

  if (params->vibrance_offset_ >= 0.0f) {
    const float luma = px.x * 0.299f + px.y * 0.587f + px.z * 0.114f;
    px.x = luma + (px.x - luma) * scale;
    px.y = luma + (px.y - luma) * scale;
    px.z = luma + (px.z - luma) * scale;
  } else {
    const float avg = (px.x + px.y + px.z) / 3.0f;
    px.x += (avg - px.x) * (1.0f - scale);
    px.y += (avg - px.y) * (1.0f - scale);
    px.z += (avg - px.z) * (1.0f - scale);
  }
  return px;
}

// === Color Wheel (Lift / Gamma / Gain) ========================================

static inline float4 opencl_color_wheel_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->color_wheel_enabled_ == 0u) return px;

  const float offset_r = params->lift_color_offset_[0] + params->lift_luminance_offset_;
  const float offset_g = params->lift_color_offset_[1] + params->lift_luminance_offset_;
  const float offset_b = params->lift_color_offset_[2] + params->lift_luminance_offset_;

  const float slope_r = fmax(params->gain_color_offset_[0] + params->gain_luminance_offset_, 1e-6f);
  const float slope_g = fmax(params->gain_color_offset_[1] + params->gain_luminance_offset_, 1e-6f);
  const float slope_b = fmax(params->gain_color_offset_[2] + params->gain_luminance_offset_, 1e-6f);

  const float power_r = fmax(params->gamma_color_offset_[0] + params->gamma_luminance_offset_, 1e-6f);
  const float power_g = fmax(params->gamma_color_offset_[1] + params->gamma_luminance_offset_, 1e-6f);
  const float power_b = fmax(params->gamma_color_offset_[2] + params->gamma_luminance_offset_, 1e-6f);

  const float base_r = fmax(px.x * slope_r + offset_r, 0.0f);
  const float base_g = fmax(px.y * slope_g + offset_g, 0.0f);
  const float base_b = fmax(px.z * slope_b + offset_b, 0.0f);

  px.x = pow(base_r, power_r);
  px.y = pow(base_g, power_g);
  px.z = pow(base_b, power_b);
  return px;
}

// === HLS ======================================================================

static inline float4 opencl_hls_op(float4 px, __global const OpenClFusedParams* params) {
  if (params->hls_enabled_ == 0u && params->saturation_enabled_ == 0u) return px;

  const float kEps = 1e-6f;
  const float kPi  = 3.14159265358979323846f;
  const float3 source_ap1 = opencl_acescc_to_ap1(px.xyz);
  const float3 source_lab = opencl_ap1_to_oklab(source_ap1);
  const float source_chroma = hypot(source_lab.y, source_lab.z);
  if (source_chroma <= kEps) return px;

  const float source_hue = opencl_wrap_hue(atan2(source_lab.z, source_lab.y) * (180.0f / kPi));

  float3 curve = (float3)(0.0f, 0.0f, 0.0f);
  if (params->hls_enabled_ != 0u) {
    int profile_count = params->hls_profile_count_;
    if (profile_count < 1) profile_count = 1;
    if (profile_count > ALCEDO_OPENCL_HLS_PROFILE_COUNT) profile_count = ALCEDO_OPENCL_HLS_PROFILE_COUNT;

    float accum_h = 0.0f;
    float accum_l = 0.0f;
    float accum_c = 0.0f;
    float accum_weight = 0.0f;
    int nearest = 0;
    float nearest_dist = opencl_hue_distance(source_hue, opencl_wrap_hue(params->hls_profile_hues_[0]));

    for (int i = 0; i < ALCEDO_OPENCL_HLS_PROFILE_COUNT; ++i) {
      if (i >= profile_count) continue;

      const float width     = fmax(params->hls_profile_hue_ranges_[i], 1.0f);
      const float target_h  = opencl_wrap_hue(params->hls_profile_hues_[i]);
      const float hue_dist  = opencl_hue_distance(source_hue, target_h);
      if (hue_dist < nearest_dist) {
        nearest_dist = hue_dist;
        nearest = i;
      }

      const float t = hue_dist / width;
      const float weight = exp2(-(t * t));
      accum_h += params->hls_profile_adjustments_[i][0] * weight;
      accum_l += params->hls_profile_adjustments_[i][1] * weight;
      accum_c += params->hls_profile_adjustments_[i][2] * weight;
      accum_weight += weight;
    }

    curve = (float3)(params->hls_profile_adjustments_[nearest][0],
                     params->hls_profile_adjustments_[nearest][1],
                     params->hls_profile_adjustments_[nearest][2]);
    if (accum_weight > kEps) {
      curve = (float3)(accum_h, accum_l, accum_c) / accum_weight;
    }
  }
  const float saturation_scale =
      (params->saturation_enabled_ != 0u) ? fmax(params->saturation_offset_, 0.0f) : 1.0f;
  if (fabs(curve.x) <= kEps && fabs(curve.y) <= kEps && fabs(curve.z) <= kEps &&
      fabs(saturation_scale - 1.0f) <= kEps) return px;

  const bool has_curve =
      fabs(curve.x) > kEps || fabs(curve.y) > kEps || fabs(curve.z) > kEps;
  float protection = 0.0f;
  if (has_curve) {
    const float chroma_confidence = opencl_smoothstep_range(0.005f, 0.030f, source_chroma);
    const float shadow_confidence = opencl_smoothstep_range(0.005f, 0.050f, source_lab.x);
    const float highlight_confidence = 1.0f - opencl_smoothstep_range(1.35f, 2.25f, source_lab.x);
    protection = clamp(chroma_confidence * shadow_confidence * highlight_confidence, 0.0f, 1.0f);
  }
  if (protection <= kEps && fabs(saturation_scale - 1.0f) <= kEps) return px;

  const float curve_gain = 2.25f;
  const float adjusted_hue_rad =
      opencl_wrap_hue(source_hue + curve.x * curve_gain * protection) * (kPi / 180.0f);
  const float adjusted_lightness =
      (fabs(curve.y) > kEps && protection > kEps)
          ? opencl_soft_floor(source_lab.x + curve.y * curve_gain * 0.5f * protection, 0.0f,
                              0.02f)
          : source_lab.x;
  const float chroma_strength = (curve.z >= 0.0f) ? 4.5f : 3.25f;
  const float adjusted_chroma =
      source_chroma * saturation_scale * exp2(curve.z * curve_gain * chroma_strength * protection);

  const float3 adjusted_lab =
      (float3)(adjusted_lightness, adjusted_chroma * cos(adjusted_hue_rad),
               adjusted_chroma * sin(adjusted_hue_rad));
  const float3 neutral_lab = (float3)(adjusted_lightness, 0.0f, 0.0f);
  const float3 output_ap1 =
      opencl_fit_ap1_lower_gamut(opencl_oklab_to_ap1(adjusted_lab),
                                 opencl_oklab_to_ap1(neutral_lab));
  px.xyz = opencl_ap1_to_acescc(output_ap1);

  return px;
}

#endif  // ALCEDO_OPENCL_EDIT_PIPELINE_COLOR_CL
