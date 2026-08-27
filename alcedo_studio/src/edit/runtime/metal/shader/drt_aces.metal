//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

static inline bool isfinite_f(float x) { return isfinite(x); }
static inline float clamp_f(float x, float lo, float hi) { return fmin(hi, fmax(lo, x)); }
static inline int clamp_i(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float safe_sqrt(float x) { return sqrt(fmax(x, 0.0f)); }

static inline float safe_div(float a, float b, float eps = 1e-7f) {
  const float ab = fabs(b);
  const float bb = (ab < eps) ? copysign(eps, b) : b;
  return a / bb;
}

static inline float safe_log10_ratio(float num, float den, float eps = 1e-7f) {
  return log10(fmax(num, eps) / fmax(den, eps));
}

static inline float safe_pow_pos(float base, float expv) { return pow(fmax(base, 0.0f), expv); }

struct HueDependentGamutParams {
  float2 JMcusp;
  float  gamma_bottom_inv;
  float  gamma_top_inv;
  float  focus_J;
  float  analytical_threshold;
};

static inline float wrap_to_360(float hue) {
  if (!isfinite_f(hue)) return 0.0f;
  float y = fmod(hue, 360.0f);
  if (y < 0.0f) y += 360.0f;
  return y;
}

static inline float radians_to_degrees(float rad) { return rad * (180.0f / kOpenDrtPi); }
static inline float degrees_to_radians(float deg) { return deg * (kOpenDrtPi / 180.0f); }

static inline int hue_position_in_uniform_table(float hue, int table_size) {
  const float wrapped = wrap_to_360(hue);
  const float pos     = wrapped * (float(table_size) / kMetalHueLimit);
  return clamp_i(int(pos), 0, table_size - 1);
}

static inline float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

static inline float reach_M_from_table(float h, const constant MetalODTParams& p) {
  const float hw   = wrap_to_360(h);
  const float pos  = hw * (float(kMetalOdtTableSize) / kMetalHueLimit);
  const int   base = clamp_i(int(pos), 0, kMetalOdtTableSize - 1);
  const float t    = clamp_f(pos - float(base), 0.0f, 1.0f);
  const int   i_lo = base + kMetalOdtBaseIndex;
  const int   i_hi = i_lo + 1;
  return lerp_f(p.table_reach_M_[i_lo], p.table_reach_M_[i_hi], t);
}

static inline float pacrc_fwd_base(float rc) {
  const float fl_y = pow(rc, 0.42f);
  return fl_y / (kCamNlOffset + fl_y);
}

static inline float pacrc_fwd(float v) {
  return copysign(pacrc_fwd_base(fabs(v)), v);
}

static inline float pacrc_inv_base(float ra) {
  const float ra_lim = fmin(ra, 0.99f);
  const float fl_y   = (kCamNlOffset * ra_lim) / (1.0f - ra_lim);
  return pow(fl_y, 1.0f / 0.42f);
}

static inline float pacrc_inv(float v) {
  return copysign(pacrc_inv_base(fabs(v)), v);
}

static inline float Achromatic_n_to_J(float a, float cz) { return kJScale * pow(a, cz); }
static inline float J_to_Achromatic_n(float j, float inv_cz) { return pow(j * (1.0f / kJScale), inv_cz); }

static inline float3 RGB_to_Aab(float3 rgb, const constant MetalJMhParams& p) {
  const float3 rgb_m = mult_f3_f33(rgb, p.MATRIX_RGB_to_CAM16_c_);
  const float3 rgb_a = float3(pacrc_fwd(rgb_m.x), pacrc_fwd(rgb_m.y), pacrc_fwd(rgb_m.z));
  return mult_f3_f33(rgb_a, p.MATRIX_cone_response_to_Aab_);
}

static inline float3 Aab_to_JMh(float3 aab, const constant MetalJMhParams& p) {
  const float mask  = aab.x > 0.0f ? 1.0f : 0.0f;
  const float j     = Achromatic_n_to_J(aab.x, p.cz_) * mask;
  const float m     = safe_sqrt(aab.y * aab.y + aab.z * aab.z) * mask;
  const float h_rad = atan2(aab.z, aab.y);
  const float h     = wrap_to_360(radians_to_degrees(h_rad)) * mask;
  return float3(j, m, h);
}

static inline float3 JMh_to_Aab(float3 jmh, const constant MetalJMhParams& p) {
  const float h_rad = degrees_to_radians(jmh.z);
  return float3(J_to_Achromatic_n(jmh.x, p.inv_cz_), jmh.y * cos(h_rad), jmh.y * sin(h_rad));
}

static inline float3 Aab_to_RGB(float3 aab, const constant MetalJMhParams& p) {
  const float3 rgb_a = mult_f3_f33(aab, p.MATRIX_Aab_to_cone_response_);
  const float3 rgb_m = float3(pacrc_inv(rgb_a.x), pacrc_inv(rgb_a.y), pacrc_inv(rgb_a.z));
  return mult_f3_f33(rgb_m, p.MATRIX_CAM16_c_to_RGB_);
}

static inline float3 RGB_to_JMh(float3 color, const constant MetalJMhParams& p) {
  return Aab_to_JMh(RGB_to_Aab(color, p), p);
}

static inline float3 JMh_to_RGB(float3 jmh, const constant MetalJMhParams& p) {
  return Aab_to_RGB(JMh_to_Aab(jmh, p), p);
}

static inline float A_to_Y(float a, const constant MetalJMhParams& p) {
  return pacrc_inv_base(p.A_w_J_ * a) / p.F_L_n_;
}

static inline float J_to_Y(float j, const constant MetalJMhParams& p) {
  return A_to_Y(J_to_Achromatic_n(fabs(j), p.inv_cz_), p);
}

static inline float Y_to_J(float y, const constant MetalJMhParams& p) {
  const float ra = pacrc_fwd_base(fabs(y) * p.F_L_n_);
  const float j  = Achromatic_n_to_J(ra * p.inv_A_w_J_, p.cz_);
  return copysign(j, y);
}

static inline float chroma_compress_norm(float h, float chroma_compress_scale) {
  const float hr      = degrees_to_radians(h);
  const float a       = cos(hr);
  const float b       = sin(hr);
  const float cos_hr2 = a * a - b * b;
  const float sin_hr2 = 2.0f * a * b;
  const float cos_hr3 = 4.0f * a * a * a - 3.0f * a;
  const float sin_hr3 = 3.0f * b - 4.0f * b * b * b;
  const float m       = 11.34072f * a + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 +
                  14.66441f * b - 6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f;
  return m * chroma_compress_scale;
}

static inline float reinhard_remap(float scale, float nd, bool invert = false) {
  if (invert) {
    if (nd >= 1.0f) return scale;
    return scale * -(nd / (nd - 1.0f));
  }
  return scale * nd / (1.0f + nd);
}

static inline float toe(float x, float limit, float k1_in, float k2_in, bool invert = false) {
  if (x > limit) return x;
  const float k2 = fmax(k2_in, 0.001f);
  const float k1 = sqrt(k1_in * k1_in + k2 * k2);
  const float k3 = (limit + k1) / (limit + k2);
  if (invert) {
    return (x * x + k1 * x) / (k3 * (x + k2));
  }
  const float minus_b = k3 * x - k1;
  const float minus_c = k2 * k3 * x;
  return 0.5f * (minus_b + safe_sqrt(minus_b * minus_b + 4.0f * minus_c));
}

static inline float3 chroma_compress_fwd(float3 jmh, float tonemapped_j, const constant MetalODTParams& p,
                                         bool invert = false) {
  float m_compr = jmh.y;
  if (jmh.y != 0.0f) {
    const float limit_j          = fmax(p.limit_J_max, 1e-6f);
    const float jts              = fmax(tonemapped_j, 0.0f);
    const float nj               = clamp_f(jts / limit_j, 0.0f, 1.0f);
    const float snj              = fmax(0.0f, 1.0f - nj);
    const float mnorm            = chroma_compress_norm(jmh.z, p.chroma_compress_scale);
    if (!isfinite_f(mnorm) || fabs(mnorm) < 1e-6f) {
      return float3(jts, 0.0f, jmh.z);
    }
    const float limit            = fmax(safe_pow_pos(nj, p.model_gamma_inv) * reach_M_from_table(jmh.z, p) / mnorm,
                               0.0f);
    const float toe_limit        = limit - 0.001f;
    const float toe_snj_sat      = snj * p.sat;
    const float toe_sqrt_nj_thr  = sqrt(nj * nj + p.sat_thr);
    const float toe_nj_compr     = nj * p.compr;
    const float ratio            = (fabs(jmh.x) < 1e-6f) ? 1.0f : (jts / fabs(jmh.x));
    m_compr                      = jmh.y * safe_pow_pos(ratio, p.model_gamma_inv);
    m_compr                      = m_compr / mnorm;
    m_compr                      = limit - toe(limit - m_compr, toe_limit, toe_snj_sat, toe_sqrt_nj_thr, false);
    m_compr                      = toe(m_compr, limit, toe_nj_compr, snj, false);
    m_compr                      = m_compr * mnorm;
  }
  (void)invert;
  return float3(tonemapped_j, m_compr, jmh.z);
}

static inline float3 tonemap_and_compress_fwd(float3 jmh, const constant MetalODTParams& p) {
  const float linear       = J_to_Y(jmh.x, p.input_params_) / kRefLuminance;
  const float tonemapped_y = Tonescale_fwd(linear, p.ts_);
  const float j_ts         = Y_to_J(tonemapped_y, p.input_params_);
  return chroma_compress_fwd(jmh, j_ts, p);
}

static inline float gamut_cusp_hue(constant MetalODTParams& p, int index) {
  return p.table_gamut_cusps_[index][2];
}

static inline float2 cusp_from_table(float h, const constant MetalODTParams& p) {
  const float hw = wrap_to_360(h);
  int low_i      = 0;
  int high_i     = kMetalOdtBaseIndex + kMetalOdtTableSize;
  int i          = kMetalOdtBaseIndex + hue_position_in_uniform_table(hw, kMetalOdtTableSize);
  for (int k = 0; k < 10 && (low_i + 1 < high_i); ++k) {
    const float h_i = gamut_cusp_hue(p, i);
    if (hw > h_i) {
      low_i = i;
    } else {
      high_i = i;
    }
    i = (low_i + high_i) >> 1;
  }
  const int lo_idx = high_i - 1;
  const int hi_idx = high_i;
  const float lo_h = p.table_gamut_cusps_[lo_idx][2];
  const float hi_h = p.table_gamut_cusps_[hi_idx][2];
  const float denom = hi_h - lo_h;
  const float t     = (denom != 0.0f) ? (hw - lo_h) / denom : 0.0f;
  return float2(lerp_f(p.table_gamut_cusps_[lo_idx][0], p.table_gamut_cusps_[hi_idx][0], t),
                lerp_f(p.table_gamut_cusps_[lo_idx][1], p.table_gamut_cusps_[hi_idx][1], t));
}

static inline float compute_focus_J(float cusp_j, float mid_j, float limit_j_max) {
  return lerp_f(cusp_j, mid_j, fmin(1.0f, kCuspMidBlend - (cusp_j / limit_j_max)));
}

static inline int look_hue_interval(float h, const constant MetalODTParams& p) {
  const float hw = wrap_to_360(h);
  int i          = kMetalOdtBaseIndex + hue_position_in_uniform_table(hw, kMetalOdtTableSize);
  int i_lo       = i + p.hue_linearity_search_range[0];
  int i_hi       = i + p.hue_linearity_search_range[1];
  i_lo           = i_lo < kMetalOdtBaseIndex ? kMetalOdtBaseIndex : i_lo;
  i_hi           = i_hi > (kMetalOdtBaseIndex + kMetalOdtTableSize)
                       ? (kMetalOdtBaseIndex + kMetalOdtTableSize)
                       : i_hi;
  i              = (i_lo + i_hi) >> 1;
  for (int k = 0; k < 6 && (i_lo + 1 < i_hi); ++k) {
    const float v = p.table_hues_[i];
    if (hw > v) {
      i_lo = i;
    } else {
      i_hi = i;
    }
    i = (i_lo + i_hi) >> 1;
  }
  return (i_hi < 1) ? 1 : i_hi;
}

static inline HueDependentGamutParams init_HueDependentGamutParams(float h, const constant MetalODTParams& p) {
  HueDependentGamutParams hdp;
  hdp.gamma_bottom_inv = p.lower_hull_gamma_inv;
  const int i_hi       = look_hue_interval(h, p);
  const float hw       = wrap_to_360(h);
  const float t        = hw - p.table_hues_[i_hi - 1];
  hdp.JMcusp           = cusp_from_table(h, p);
  hdp.gamma_top_inv    = lerp_f(p.table_upper_hull_gamma_[i_hi - 1], p.table_upper_hull_gamma_[i_hi], t);
  hdp.focus_J          = compute_focus_J(hdp.JMcusp.x, p.mid_J, p.limit_J_max);
  hdp.analytical_threshold = lerp_f(hdp.JMcusp.x, p.limit_J_max, kFocusGainBlend);
  return hdp;
}

static inline float get_focus_gain(float j, float analytical_threshold, float limit_j_max, float focus_dist) {
  float gain = limit_j_max * focus_dist;
  if (j > analytical_threshold) {
    float gain_adjustment = safe_log10_ratio(limit_j_max - analytical_threshold, limit_j_max - j, 1e-4f);
    gain_adjustment       = gain_adjustment * gain_adjustment + 1.0f;
    gain                  = gain * gain_adjustment;
  }
  return gain;
}

static inline float solve_J_intersect(float j, float m, float focus_j, float max_j, float slope_gain) {
  const float sg       = fmax(fabs(slope_gain), 1e-6f);
  const float fj       = fmax(fabs(focus_j), 1e-6f);
  const float m_scaled = m / sg;
  const float a        = m_scaled / fj;
  const float b1       = 1.0f - m_scaled;
  const float c1       = -j;
  const float det1     = b1 * b1 - 4.0f * a * c1;
  const float r1       = (det1 > 0.0f) ? safe_sqrt(det1) : 0.0f;
  const float den1     = copysign(fmax(fabs(b1 + r1), 1e-6f), b1 + r1);
  const float res1     = (-2.0f * c1) / den1;
  const float b2       = -(1.0f + m_scaled + max_j * a);
  const float c2       = max_j * m_scaled + j;
  const float det2     = b2 * b2 - 4.0f * a * c2;
  const float r2       = (det2 > 0.0f) ? safe_sqrt(det2) : 0.0f;
  const float den2     = copysign(fmax(fabs(b2 - r2), 1e-6f), b2 - r2);
  const float res2     = (-2.0f * c2) / den2;
  return (j < focus_j) ? res1 : res2;
}

static inline float compute_compression_vector_slope(float intersect_j, float focus_j, float limit_j_max,
                                                     float slope_gain) {
  const float direction_scalar = (intersect_j < focus_j) ? intersect_j : (limit_j_max - intersect_j);
  const float denom            = fmax(fabs(focus_j * slope_gain), 1e-6f);
  return direction_scalar * (intersect_j - focus_j) / denom;
}

static inline float estimate_line_and_boundary_intersection_M(float j_axis_intersect, float slope,
                                                              float inv_gamma, float j_max, float m_max,
                                                              float j_intersection_reference) {
  const float refj                 = fmax(j_intersection_reference, 1e-6f);
  const float normalized_j         = fmax(j_axis_intersect / refj, 0.0f);
  const float shifted_intersection = refj * safe_pow_pos(normalized_j, inv_gamma);
  const float denom                = copysign(fmax(fabs(j_max - slope * m_max), 1e-6f), (j_max - slope * m_max));
  return shifted_intersection * m_max / denom;
}

static inline float smin_scaled(float a, float b, float scale_reference) {
  const float s_scaled = kSmoothCusps * scale_reference;
  if (s_scaled <= 1e-6f) return fmin(a, b);
  const float h = fmax(s_scaled - fabs(a - b), 0.0f) / s_scaled;
  return fmin(a, b) - h * h * h * s_scaled * (1.0f / 6.0f);
}

static inline float find_gamut_boundary_intersection(float2 jm_cusp, float j_max, float gamma_top_inv,
                                                     float gamma_bottom_inv, float j_intersect_source,
                                                     float slope, float j_intersect_cusp) {
  const float m_boundary_lower = estimate_line_and_boundary_intersection_M(
      j_intersect_source, slope, gamma_bottom_inv, jm_cusp.x, jm_cusp.y, j_intersect_cusp);
  const float f_j_intersect_cusp   = j_max - j_intersect_cusp;
  const float f_j_intersect_source = j_max - j_intersect_source;
  const float f_jm_cusp_j          = j_max - jm_cusp.x;
  const float m_boundary_upper     = estimate_line_and_boundary_intersection_M(
      f_j_intersect_source, -slope, gamma_top_inv, f_jm_cusp_j, jm_cusp.y, f_j_intersect_cusp);
  return smin_scaled(m_boundary_lower, m_boundary_upper, jm_cusp.y);
}

static inline float remap_M(float m, float gamut_boundary_m, float reach_boundary_m, bool invert = false) {
  const float boundary_ratio = safe_div(gamut_boundary_m, reach_boundary_m, 1e-6f);
  const float proportion     = fmax(boundary_ratio, kCompressionThreshold);
  const float threshold      = proportion * gamut_boundary_m;
  if (m <= threshold || proportion >= 1.0f) return m;
  const float m_offset       = m - threshold;
  const float gamut_off      = gamut_boundary_m - threshold;
  const float reach_off      = reach_boundary_m - threshold;
  const float ratio_rg       = safe_div(reach_off, gamut_off, 1e-6f);
  const float denom          = fmax(ratio_rg - 1.0f, 1e-7f);
  const float scale          = reach_off / denom;
  const float nd             = m_offset / scale;
  return threshold + reinhard_remap(scale, nd, invert);
}

static inline float3 compress_gamut(float3 jmh, float jx, const constant MetalODTParams& p,
                                    HueDependentGamutParams hdp, bool invert = false) {
  const float slope_gain = fmax(get_focus_gain(jx, hdp.analytical_threshold, p.limit_J_max, p.focus_dist), 1e-6f);
  const float j_intersect_source = solve_J_intersect(jmh.x, jmh.y, hdp.focus_J, p.limit_J_max, slope_gain);
  const float gamut_slope =
      compute_compression_vector_slope(j_intersect_source, hdp.focus_J, p.limit_J_max, slope_gain);
  const float j_intersect_cusp =
      solve_J_intersect(hdp.JMcusp.x, hdp.JMcusp.y, hdp.focus_J, p.limit_J_max, slope_gain);
  const float gamut_boundary_m = find_gamut_boundary_intersection(
      hdp.JMcusp, p.limit_J_max, hdp.gamma_top_inv, hdp.gamma_bottom_inv, j_intersect_source,
      gamut_slope, j_intersect_cusp);
  if (gamut_boundary_m <= 0.0f) {
    return float3(jx, 0.0f, jmh.z);
  }
  const float reach_max_m      = fmax(reach_M_from_table(jmh.z, p), 0.0f);
  const float reach_boundary_m = estimate_line_and_boundary_intersection_M(
      j_intersect_source, gamut_slope, p.model_gamma_inv, p.limit_J_max, reach_max_m, p.limit_J_max);
  const float remapped_m       = remap_M(jmh.y, gamut_boundary_m, reach_boundary_m, invert);
  return float3(j_intersect_source + remapped_m * gamut_slope, remapped_m, jmh.z);
}

static inline float3 gamut_compress_fwd(float3 jmh, const constant MetalODTParams& p) {
  if (jmh.x <= 0.0f) {
    return float3(0.0f, 0.0f, jmh.z);
  }
  if (jmh.y <= 0.0f || jmh.x > p.limit_J_max) {
    return float3(jmh.x, 0.0f, jmh.z);
  }
  return compress_gamut(jmh, jmh.x, p, init_HueDependentGamutParams(jmh.z, p), false);
}

static inline float3 limit_rgb_preserve_chroma(float3 rgb, float lower, float upper) {
  if (!all(isfinite(rgb))) {
    return float3(0.0f);
  }
  rgb = max(rgb, float3(lower));
  const float m = fmax(rgb.x, fmax(rgb.y, rgb.z));
  if (m > upper && m > 0.0f) {
    rgb *= upper / m;
  }
  return rgb;
}

static inline float3 OutputTransform_fwd(float3 in_color, constant MetalODTParams& p) {
  if (!all(isfinite(in_color))) {
    return float3(0.0f);
  }
  const float3 ap0            = mult_f3_f33(in_color, kAp1ToAp0);
  const float3 jmh            = RGB_to_JMh(ap0, p.input_params_);
  const float3 tonemapped_jmh = tonemap_and_compress_fwd(jmh, p);
  const float3 compressed_jmh = gamut_compress_fwd(tonemapped_jmh, p);
  const float3 out_rgb        = JMh_to_RGB(compressed_jmh, p.limit_params_);
  return limit_rgb_preserve_chroma(out_rgb, 0.0f, p.ts_.forward_limit_);
}

