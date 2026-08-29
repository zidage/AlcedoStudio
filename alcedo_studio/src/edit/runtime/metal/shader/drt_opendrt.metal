//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

static inline float3 odrt_apply_matrix(constant float* mat, float3 v) {
  return apply_matrix3x3(mat, v);
}

static inline float3 odrt_add_scalar(float3 v, float s) { return v + s; }
static inline float3 odrt_add(float3 a, float3 b) { return a + b; }
static inline float3 odrt_sub(float3 a, float3 b) { return a - b; }
static inline float3 odrt_mul_scalar(float3 v, float s) { return v * s; }
static inline float3 odrt_mul(float3 a, float3 b) { return a * b; }

static inline float3 odrt_div_scalar(float3 v, float s) {
  if (fabs(s) < 1e-8f) return float3(0.0f);
  return v / s;
}

static inline float odrt_spowf(float a, float b) { return (a <= 0.0f) ? a : pow(a, b); }
static inline float3 odrt_spowf3(float3 a, float b) {
  return float3(odrt_spowf(a.x, b), odrt_spowf(a.y, b), odrt_spowf(a.z, b));
}
static inline float odrt_hypot2(float2 v) { return sqrt(fmax(0.0f, dot(v, v))); }
static inline float odrt_hypot3(float3 v) { return sqrt(fmax(0.0f, dot(v, v))); }
static inline float3 odrt_clampf3(float3 v, float mn, float mx) { return clamp(v, float3(mn), float3(mx)); }
static inline float3 odrt_clampminf3(float3 v, float mn) { return max(v, float3(mn)); }
static inline float odrt_compress_hyperbolic_power(float x, float s, float p) { return odrt_spowf(x / (x + s), p); }

static inline float odrt_compress_toe_quadratic(float x, float toe, bool inv = false) {
  if (toe == 0.0f) return x;
  if (!inv) return odrt_spowf(x, 2.0f) / (x + toe);
  return (x + sqrt(x * (4.0f * toe + x))) / 2.0f;
}

static inline float odrt_compress_toe_cubic(float x, float m, float w, bool inv = false) {
  if (m == 1.0f) return x;
  const float x2 = x * x;
  if (!inv) return x * (x2 + m * w) / (x2 + w);
  const float p0 = x2 - 3.0f * m * w;
  const float p1 = 2.0f * x2 + 27.0f * w - 9.0f * m * w;
  const float p2 =
      pow(sqrt(x2 * p1 * p1 - 4.0f * p0 * p0 * p0) / 2.0f + x * p1 / 2.0f, 1.0f / 3.0f);
  return p0 / (3.0f * p2) + p2 / 3.0f + x / 3.0f;
}

static inline float odrt_contrast_high(float x, float p, float pv, float pv_lx, bool inv = false) {
  const float x0 = 0.18f * pow(2.0f, pv);
  if (x < x0 || p == 1.0f) return x;
  const float o  = x0 - x0 / p;
  const float s0 = pow(x0, 1.0f - p) / p;
  const float x1 = x0 * pow(2.0f, pv_lx);
  const float k1 = p * s0 * pow(x1, p) / x1;
  const float y1 = s0 * pow(x1, p) + o;
  if (inv) return (x > y1) ? (x - y1) / k1 + x1 : pow((x - o) / s0, 1.0f / p);
  return (x > x1) ? k1 * (x - x1) + y1 : s0 * pow(x, p) + o;
}

static inline float odrt_softplus(float x, float s) {
  if (x > 10.0f * s || s < 1e-4f) return x;
  return s * log(fmax(0.0f, 1.0f + exp(x / s)));
}

static inline float odrt_gauss_window(float x, float w) { return exp(-x * x / w); }
static inline float2 odrt_opponent(float3 rgb) { return float2(rgb.x - rgb.z, rgb.y - (rgb.x + rgb.z) / 2.0f); }
static inline float odrt_hue_offset(float h, float o) { return fmod(h - o + kOpenDrtPi, 2.0f * kOpenDrtPi) - kOpenDrtPi; }

static inline float3 odrt_display_gamut_whitepoint(float3 rgb, float tsn, float cwp_lm, int display_gamut, int cwp) {
  rgb                 = odrt_apply_matrix(kOpenDrtP3D65ToXyz, rgb);
  float3 cwp_neutral  = rgb;
  const float cwp_f   = pow(tsn, 2.0f * cwp_lm);
  if (display_gamut < 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD75, rgb);
    else if (cwp == 3) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatD65ToD50, rgb);
  } else if (display_gamut == 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD75, rgb);
    else if (cwp == 2) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD65, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatD60ToD50, rgb);
  } else {
    cwp_neutral = odrt_apply_matrix(kOpenDrtCatDciToD65, rgb);
    if (cwp == 0) rgb = odrt_apply_matrix(kOpenDrtCatDciToD93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(kOpenDrtCatDciToD75, rgb);
    else if (cwp == 2) rgb = cwp_neutral;
    else if (cwp == 3) rgb = odrt_apply_matrix(kOpenDrtCatDciToD60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(kOpenDrtCatDciToD55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(kOpenDrtCatDciToD50, rgb);
  }
  rgb = odrt_add(odrt_mul_scalar(rgb, cwp_f), odrt_mul_scalar(cwp_neutral, 1.0f - cwp_f));
  if (display_gamut == 0) {
    rgb = odrt_apply_matrix(kOpenDrtXyzToRec709, rgb);
  } else if (display_gamut == 5) {
    rgb = odrt_apply_matrix(kOpenDrtCatD65ToDci, rgb);
  } else {
    rgb = odrt_apply_matrix(kOpenDrtXyzToP3D65, rgb);
  }
  float cwp_norm = 1.0f;
  if (display_gamut == 0) {
    if (cwp == 0) cwp_norm = 0.7441926991f;
    else if (cwp == 1) cwp_norm = 0.8734708321f;
    else if (cwp == 3) cwp_norm = 0.9559369922f;
    else if (cwp == 4) cwp_norm = 0.9056713328f;
    else if (cwp == 5) cwp_norm = 0.8500043850f;
  } else if (display_gamut == 1 || display_gamut == 2) {
    if (cwp == 0) cwp_norm = 0.7626870573f;
    else if (cwp == 1) cwp_norm = 0.8840540833f;
    else if (cwp == 3) cwp_norm = 0.9643201867f;
    else if (cwp == 4) cwp_norm = 0.9230765189f;
    else if (cwp == 5) cwp_norm = 0.8765728378f;
  } else if (display_gamut == 3) {
    if (cwp == 0) cwp_norm = 0.7049563210f;
    else if (cwp == 1) cwp_norm = 0.8167157098f;
    else if (cwp == 2) cwp_norm = 0.9233821937f;
    else if (cwp == 4) cwp_norm = 0.9561385003f;
    else if (cwp == 5) cwp_norm = 0.9068014530f;
  } else if (display_gamut == 4) {
    if (cwp == 0) cwp_norm = 0.6653361412f;
    else if (cwp == 1) cwp_norm = 0.7703971314f;
    else if (cwp == 2) cwp_norm = 0.8705723433f;
    else if (cwp == 3) cwp_norm = 0.8913545475f;
    else if (cwp == 4) cwp_norm = 0.8553278252f;
    else if (cwp == 5) cwp_norm = 0.8145664361f;
  } else if (display_gamut == 5) {
    if (cwp == 0) cwp_norm = 0.7071427840f;
    else if (cwp == 1) cwp_norm = 0.8155610826f;
    else if (cwp >= 2) cwp_norm = 0.9165552797f;
  }
  return odrt_mul_scalar(rgb, cwp_norm * cwp_f + 1.0f - cwp_f);
}

static inline float3 OpenDRTTransform_fwd(float3 input_color, const constant MetalOpenDRTParams& p) {
  float3 rgb           = odrt_apply_matrix(kOpenDrtAp1ToXyz, input_color);
  rgb                  = odrt_apply_matrix(kOpenDrtXyzToP3D65, rgb);
  const float3 rs_w    = float3(p.rs_rw_, 1.0f - p.rs_rw_ - p.rs_bw_, p.rs_bw_);
  float sat_l          = dot(rgb, rs_w);
  rgb                  = odrt_add(odrt_mul_scalar(float3(sat_l), p.rs_sa_), odrt_mul_scalar(rgb, 1.0f - p.rs_sa_));
  rgb                  = odrt_add_scalar(rgb, p.tn_off_);
  float tsn            = odrt_hypot3(rgb) / kOpenDrtSqrt3;
  rgb                  = odrt_div_scalar(rgb, tsn);
  const float2 opp     = odrt_opponent(rgb);
  float ach_d          = odrt_hypot2(opp) / 2.0f;
  ach_d                = 1.25f * odrt_compress_toe_quadratic(ach_d, 0.25f, false);
  const float hue      = fmod(atan2(opp.x, opp.y) + kOpenDrtPi + 1.10714931f, 2.0f * kOpenDrtPi);
  const float3 ha_rgb  = float3(odrt_gauss_window(odrt_hue_offset(hue, 0.1f), 0.66f),
                                odrt_gauss_window(odrt_hue_offset(hue, 4.3f), 0.66f),
                                odrt_gauss_window(odrt_hue_offset(hue, 2.3f), 0.66f));
  const float3 ha_rgb_hs = float3(odrt_gauss_window(odrt_hue_offset(hue, -0.4f), 0.66f), ha_rgb.y,
                                  odrt_gauss_window(odrt_hue_offset(hue, 2.5f), 0.66f));
  const float3 ha_cmy    = float3(odrt_gauss_window(odrt_hue_offset(hue, 3.3f), 0.5f),
                               odrt_gauss_window(odrt_hue_offset(hue, 1.3f), 0.5f),
                               odrt_gauss_window(odrt_hue_offset(hue, -1.15f), 0.5f));
  if (p.brl_enable_) {
    const float brl_tsf = pow(tsn / (tsn + 1.0f), 1.0f - p.brl_rng_);
    const float brl_exf =
        (p.brl_ + p.brl_r_ * ha_rgb.x + p.brl_g_ * ha_rgb.y + p.brl_b_ * ha_rgb.z) *
        pow(ach_d, 1.0f / p.brl_st_);
    const float brl_ex = pow(2.0f, brl_exf * ((brl_exf < 0.0f) ? brl_tsf : 1.0f - brl_tsf));
    tsn *= brl_ex;
  }
  if (p.tn_lcon_enable_) {
    const float lcon_m       = pow(2.0f, -p.tn_lcon_);
    float lcon_w             = p.tn_lcon_w_ / 4.0f;
    lcon_w                  *= lcon_w;
    const float lcon_cnst_sc = odrt_compress_toe_cubic(p.ts_x0_, lcon_m, lcon_w, true) / p.ts_x0_;
    tsn *= lcon_cnst_sc;
    tsn  = odrt_compress_toe_cubic(tsn, lcon_m, lcon_w, false);
  }
  if (p.tn_hcon_enable_) {
    tsn = odrt_contrast_high(tsn, pow(2.0f, p.tn_hcon_), p.tn_hcon_pv_, p.tn_hcon_st_, false);
  }
  const float tsn_pt    = odrt_compress_hyperbolic_power(tsn, p.ts_s1_, p.ts_p_);
  const float tsn_const = odrt_compress_hyperbolic_power(tsn, p.s_Lp100_, p.ts_p_);
  tsn                   = odrt_compress_hyperbolic_power(tsn, p.ts_s_, p.ts_p_);
  if (p.hc_enable_) {
    float hc_ts = 1.0f - tsn_const;
    float hc_c  = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
    hc_c       *= ach_d * ha_rgb.x;
    hc_ts       = pow(hc_ts, 1.0f / p.hc_r_rng_);
    const float hc_f = p.hc_r_ * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
    rgb              = float3(rgb.x, rgb.y * hc_f, rgb.z * hc_f);
  }
  if (p.hs_rgb_enable_) {
    const float3 hs_rgb = float3(ha_rgb_hs.x * ach_d * pow(tsn_pt, 1.0f / p.hs_r_rng_),
                                 ha_rgb_hs.y * ach_d * pow(tsn_pt, 1.0f / p.hs_g_rng_),
                                 ha_rgb_hs.z * ach_d * pow(tsn_pt, 1.0f / p.hs_b_rng_));
    const float3 hsf    = float3((hs_rgb.z * -p.hs_b_) - (hs_rgb.y * -p.hs_g_),
                              (hs_rgb.x * p.hs_r_) - (hs_rgb.z * -p.hs_b_),
                              (hs_rgb.y * -p.hs_g_) - (hs_rgb.x * p.hs_r_));
    rgb                 = odrt_add(rgb, hsf);
  }
  if (p.hs_cmy_enable_) {
    const float tsn_pt_compl = 1.0f - tsn_pt;
    const float3 hs_cmy = float3(ha_cmy.x * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_c_rng_),
                                 ha_cmy.y * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_m_rng_),
                                 ha_cmy.z * ach_d * pow(tsn_pt_compl, 1.0f / p.hs_y_rng_));
    const float3 hsf    = float3((hs_cmy.z * p.hs_y_) - (hs_cmy.y * p.hs_m_),
                              (hs_cmy.x * -p.hs_c_) - (hs_cmy.z * p.hs_y_),
                              (hs_cmy.y * p.hs_m_) - (hs_cmy.x * -p.hs_c_));
    rgb                 = odrt_add(rgb, hsf);
  }
  const float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt) *
                                    (p.pt_lml_ + p.pt_lml_r_ * ha_rgb_hs.x + p.pt_lml_g_ * ha_rgb_hs.y +
                                     p.pt_lml_b_ * ha_rgb_hs.z);
  float ptf            = 1.0f - pow(tsn_pt, pt_lml_p);
  const float pt_lmh_p = (1.0f - ach_d * (p.pt_lmh_r_ * ha_rgb_hs.x + p.pt_lmh_b_ * ha_rgb_hs.z)) *
                         (1.0f - p.pt_lmh_ * ach_d);
  ptf                  = pow(ptf, pt_lmh_p);
  if (p.ptm_enable_) {
    float ptm_low_f = 1.0f;
    if (p.ptm_low_st_ != 0.0f && p.ptm_low_rng_ != 0.0f) {
      ptm_low_f = 1.0f + p.ptm_low_ * exp(-2.0f * ach_d * ach_d / p.ptm_low_st_) *
                             pow(1.0f - tsn_const, 1.0f / p.ptm_low_rng_);
    }
    float ptm_high_f = 1.0f;
    if (p.ptm_high_st_ != 0.0f && p.ptm_high_rng_ != 0.0f) {
      ptm_high_f = 1.0f + p.ptm_high_ * exp(-2.0f * ach_d * ach_d / p.ptm_high_st_) *
                              pow(tsn_pt, 1.0f / (4.0f * p.ptm_high_rng_));
    }
    ptf *= ptm_low_f * ptm_high_f;
  }
  rgb   = odrt_add(odrt_mul_scalar(rgb, ptf), float3(1.0f - ptf));
  sat_l = dot(rgb, rs_w);
  rgb   = odrt_div_scalar(odrt_sub(odrt_mul_scalar(float3(sat_l), p.rs_sa_), rgb), p.rs_sa_ - 1.0f);
  rgb   = odrt_display_gamut_whitepoint(rgb, tsn_const, p.cwp_lm_, p.display_gamut_, p.creative_white_);
  if (p.brlp_enable_) {
    const float2 brlp_opp  = odrt_opponent(rgb);
    float brlp_ach_d       = odrt_hypot2(brlp_opp) / 4.0f;
    brlp_ach_d             = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
    const float3 brlp_ha   = odrt_mul_scalar(ha_rgb, ach_d);
    const float brlp_m     = p.brlp_ + p.brlp_r_ * brlp_ha.x + p.brlp_g_ * brlp_ha.y + p.brlp_b_ * brlp_ha.z;
    rgb                    = odrt_mul_scalar(rgb, pow(2.0f, brlp_m * brlp_ach_d * tsn));
  }
  if (p.ptl_enable_) {
    rgb = float3(odrt_softplus(rgb.x, p.ptl_c_), odrt_softplus(rgb.y, p.ptl_m_), odrt_softplus(rgb.z, p.ptl_y_));
  }
  tsn *= p.ts_m2_;
  tsn  = odrt_compress_toe_quadratic(tsn, p.tn_toe_, false);
  tsn *= p.ts_dsc_;
  rgb *= tsn;
  if (p.display_gamut_ == 2) {
    rgb = odrt_clampminf3(rgb, 0.0f);
    rgb = odrt_apply_matrix(kOpenDrtP3ToRec2020, rgb);
  }
  if (p.clamp_) {
    rgb = odrt_clampf3(rgb, 0.0f, 1.0f);
  }
  return rgb;
}

