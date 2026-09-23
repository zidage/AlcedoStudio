//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
//  This file also contains material subject to the upstream notices below.

//  This file contains GPLv3-derived logic based on OpenDRT v1.1.0
//  by Jed Smith: https://github.com/jedypod/open-display-transform

#pragma once

#include <cuda_runtime.h>
#include <vector_functions.h>
#include <vector_types.h>

#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"
#include "open_drt_const.cuh"

namespace alcedo::CUDA {

GPU_FUNC float3 odrt_apply_matrix(const float mat[9], const float3& v) {
  return make_float3(mat[0] * v.x + mat[1] * v.y + mat[2] * v.z,
                     mat[3] * v.x + mat[4] * v.y + mat[5] * v.z,
                     mat[6] * v.x + mat[7] * v.y + mat[8] * v.z);
}

GPU_FUNC float3 odrt_add_scalar(const float3& v, float s) {
  return make_float3(v.x + s, v.y + s, v.z + s);
}

GPU_FUNC float3 odrt_add(const float3& a, const float3& b) {
  return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}

GPU_FUNC float3 odrt_sub(const float3& a, const float3& b) {
  return make_float3(a.x - b.x, a.y - b.y, a.z - b.z);
}

GPU_FUNC float3 odrt_mul_scalar(const float3& v, float s) {
  return make_float3(v.x * s, v.y * s, v.z * s);
}

GPU_FUNC float3 odrt_mul(const float3& a, const float3& b) {
  return make_float3(a.x * b.x, a.y * b.y, a.z * b.z);
}

GPU_FUNC float3 odrt_div_scalar(const float3& v, float s) {
  if (fabsf(s) < 1e-8f) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }
  return make_float3(v.x / s, v.y / s, v.z / s);
}

GPU_FUNC float odrt_spowf(float a, float b) {
  if (a <= 0.0f) {
    return a;
  }
  return powf(a, b);
}

GPU_FUNC float3 odrt_spowf3(const float3& a, float b) {
  return make_float3(odrt_spowf(a.x, b), odrt_spowf(a.y, b), odrt_spowf(a.z, b));
}

GPU_FUNC float odrt_hypot2(const float2& v) {
  return sqrtf(fmaxf(0.0f, v.x * v.x + v.y * v.y));
}

GPU_FUNC float odrt_hypot3(const float3& v) {
  return sqrtf(fmaxf(0.0f, v.x * v.x + v.y * v.y + v.z * v.z));
}

GPU_FUNC float3 odrt_clampf3(const float3& v, float mn, float mx) {
  return make_float3(fminf(fmaxf(v.x, mn), mx), fminf(fmaxf(v.y, mn), mx),
                     fminf(fmaxf(v.z, mn), mx));
}

GPU_FUNC float3 odrt_clampminf3(const float3& v, float mn) {
  return make_float3(fmaxf(v.x, mn), fmaxf(v.y, mn), fmaxf(v.z, mn));
}

GPU_FUNC float odrt_compress_hyperbolic_power(float x, float s, float p) {
  return odrt_spowf(x / (x + s), p);
}

GPU_FUNC float odrt_compress_toe_quadratic(float x, float toe, int inv) {
  if (toe == 0.0f) {
    return x;
  }
  if (inv == 0) {
    return odrt_spowf(x, 2.0f) / (x + toe);
  }
  return (x + sqrtf(x * (4.0f * toe + x))) / 2.0f;
}

GPU_FUNC float odrt_compress_toe_cubic(float x, float m, float w, int inv) {
  if (m == 1.0f) {
    return x;
  }
  const float x2 = x * x;
  if (inv == 0) {
    return x * (x2 + m * w) / (x2 + w);
  }
  const float p0 = x2 - 3.0f * m * w;
  const float p1 = 2.0f * x2 + 27.0f * w - 9.0f * m * w;
  const float p2 = powf(sqrtf(x2 * p1 * p1 - 4.0f * p0 * p0 * p0) / 2.0f + x * p1 / 2.0f,
                        1.0f / 3.0f);
  return p0 / (3.0f * p2) + p2 / 3.0f + x / 3.0f;
}

GPU_FUNC float odrt_contrast_high(float x, float p, float pv, float pv_lx, int inv) {
  const float x0 = 0.18f * powf(2.0f, pv);
  if (x < x0 || p == 1.0f) {
    return x;
  }
  const float o  = x0 - x0 / p;
  const float s0 = powf(x0, 1.0f - p) / p;
  const float x1 = x0 * powf(2.0f, pv_lx);
  const float k1 = p * s0 * powf(x1, p) / x1;
  const float y1 = s0 * powf(x1, p) + o;
  if (inv == 1) {
    return (x > y1) ? (x - y1) / k1 + x1 : powf((x - o) / s0, 1.0f / p);
  }
  return (x > x1) ? k1 * (x - x1) + y1 : s0 * powf(x, p) + o;
}

GPU_FUNC float odrt_softplus(float x, float s) {
  if (x > 10.0f * s || s < 1e-4f) {
    return x;
  }
  return s * logf(fmaxf(0.0f, 1.0f + expf(x / s)));
}

GPU_FUNC float odrt_gauss_window(float x, float w) { return expf(-x * x / w); }

GPU_FUNC float2 odrt_opponent(const float3& rgb) {
  return make_float2(rgb.x - rgb.z, rgb.y - (rgb.x + rgb.z) / 2.0f);
}

GPU_FUNC float odrt_hue_offset(float h, float o) {
  return fmodf(h - o + OPEN_DRT_PI, 2.0f * OPEN_DRT_PI) - OPEN_DRT_PI;
}

GPU_FUNC float3 odrt_display_gamut_whitepoint(float3 rgb, float tsn, float cwp_lm,
                                              int display_gamut, int cwp) {
  rgb = odrt_apply_matrix(OPEN_DRT_P3D65_TO_XYZ, rgb);
  float3 cwp_neutral = rgb;
  const float cwp_f  = powf(tsn, 2.0f * cwp_lm);

  if (display_gamut < 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_D93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_D75, rgb);
    else if (cwp == 3) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_D60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_D55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_D50, rgb);
  } else if (display_gamut == 3) {
    if (cwp == 0) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D60_TO_D93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D60_TO_D75, rgb);
    else if (cwp == 2) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D60_TO_D65, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D60_TO_D55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(OPEN_DRT_CAT_D60_TO_D50, rgb);
  } else {
    cwp_neutral = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D65, rgb);
    if (cwp == 0) rgb = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D93, rgb);
    else if (cwp == 1) rgb = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D75, rgb);
    else if (cwp == 2) rgb = cwp_neutral;
    else if (cwp == 3) rgb = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D60, rgb);
    else if (cwp == 4) rgb = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D55, rgb);
    else if (cwp == 5) rgb = odrt_apply_matrix(OPEN_DRT_CAT_DCI_TO_D50, rgb);
  }

  rgb = odrt_add(odrt_mul_scalar(rgb, cwp_f), odrt_mul_scalar(cwp_neutral, 1.0f - cwp_f));

  if (display_gamut == 0) {
    rgb = odrt_apply_matrix(OPEN_DRT_XYZ_TO_REC709, rgb);
  } else if (display_gamut == 5) {
    rgb = odrt_apply_matrix(OPEN_DRT_CAT_D65_TO_DCI, rgb);
  } else {
    rgb = odrt_apply_matrix(OPEN_DRT_XYZ_TO_P3D65, rgb);
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

GPU_FUNC float3 OpenDRTTransform_fwd(const float3& input_color, const CudaDrtOpenDrtParams& p);

GPU_FUNC float3 OpenDRTTransform_fwd(const float3& input_color, const CudaDrtOpenDrtParams& p) {
  float3 rgb = odrt_apply_matrix(OPEN_DRT_AP1_TO_XYZ, input_color);
  rgb        = odrt_apply_matrix(OPEN_DRT_XYZ_TO_P3D65, rgb);

  const float3 rs_w = make_float3(p.rs_rw_, 1.0f - p.rs_rw_ - p.rs_bw_, p.rs_bw_);
  float        sat_L = rgb.x * rs_w.x + rgb.y * rs_w.y + rgb.z * rs_w.z;
  rgb               = odrt_add(odrt_mul_scalar(make_float3(sat_L, sat_L, sat_L), p.rs_sa_),
                               odrt_mul_scalar(rgb, 1.0f - p.rs_sa_));

  rgb               = odrt_add_scalar(rgb, p.tn_off_);
  float tsn         = odrt_hypot3(rgb) / OPEN_DRT_SQRT3;
  rgb               = odrt_div_scalar(rgb, tsn);

  const float2 opp  = odrt_opponent(rgb);
  float        ach_d = odrt_hypot2(opp) / 2.0f;
  ach_d             = 1.25f * odrt_compress_toe_quadratic(ach_d, 0.25f, 0);

  const float hue   = fmodf(atan2f(opp.x, opp.y) + OPEN_DRT_PI + 1.10714931f, 2.0f * OPEN_DRT_PI);

  const float3 ha_rgb = make_float3(odrt_gauss_window(odrt_hue_offset(hue, 0.1f), 0.66f),
                                    odrt_gauss_window(odrt_hue_offset(hue, 4.3f), 0.66f),
                                    odrt_gauss_window(odrt_hue_offset(hue, 2.3f), 0.66f));
  const float3 ha_rgb_hs =
      make_float3(odrt_gauss_window(odrt_hue_offset(hue, -0.4f), 0.66f), ha_rgb.y,
                  odrt_gauss_window(odrt_hue_offset(hue, 2.5f), 0.66f));
  const float3 ha_cmy =
      make_float3(odrt_gauss_window(odrt_hue_offset(hue, 3.3f), 0.5f),
                  odrt_gauss_window(odrt_hue_offset(hue, 1.3f), 0.5f),
                  odrt_gauss_window(odrt_hue_offset(hue, -1.15f), 0.5f));

  if (p.brl_enable_) {
    const float brl_tsf = powf(tsn / (tsn + 1.0f), 1.0f - p.brl_rng_);
    const float brl_exf =
        (p.brl_ + p.brl_r_ * ha_rgb.x + p.brl_g_ * ha_rgb.y + p.brl_b_ * ha_rgb.z) *
        powf(ach_d, 1.0f / p.brl_st_);
    const float brl_ex = powf(2.0f, brl_exf * ((brl_exf < 0.0f) ? brl_tsf : 1.0f - brl_tsf));
    tsn *= brl_ex;
  }

  if (p.tn_lcon_enable_) {
    const float lcon_m       = powf(2.0f, -p.tn_lcon_);
    float       lcon_w       = p.tn_lcon_w_ / 4.0f;
    lcon_w *= lcon_w;
    const float lcon_cnst_sc = odrt_compress_toe_cubic(p.ts_x0_, lcon_m, lcon_w, 1) / p.ts_x0_;
    tsn *= lcon_cnst_sc;
    tsn = odrt_compress_toe_cubic(tsn, lcon_m, lcon_w, 0);
  }

  if (p.tn_hcon_enable_) {
    const float hcon_p = powf(2.0f, p.tn_hcon_);
    tsn                = odrt_contrast_high(tsn, hcon_p, p.tn_hcon_pv_, p.tn_hcon_st_, 0);
  }

  const float tsn_pt    = odrt_compress_hyperbolic_power(tsn, p.ts_s1_, p.ts_p_);
  const float tsn_const = odrt_compress_hyperbolic_power(tsn, p.s_Lp100_, p.ts_p_);
  tsn                   = odrt_compress_hyperbolic_power(tsn, p.ts_s_, p.ts_p_);

  if (p.hc_enable_) {
    float hc_ts = 1.0f - tsn_const;
    float hc_c  = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
    hc_c *= ach_d * ha_rgb.x;
    hc_ts = powf(hc_ts, 1.0f / p.hc_r_rng_);
    const float hc_f = p.hc_r_ * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
    rgb              = make_float3(rgb.x, rgb.y * hc_f, rgb.z * hc_f);
  }

  if (p.hs_rgb_enable_) {
    const float3 hs_rgb =
        make_float3(ha_rgb_hs.x * ach_d * powf(tsn_pt, 1.0f / p.hs_r_rng_),
                    ha_rgb_hs.y * ach_d * powf(tsn_pt, 1.0f / p.hs_g_rng_),
                    ha_rgb_hs.z * ach_d * powf(tsn_pt, 1.0f / p.hs_b_rng_));
    const float3 hsf =
        make_float3((hs_rgb.z * -p.hs_b_) - (hs_rgb.y * -p.hs_g_),
                    (hs_rgb.x * p.hs_r_) - (hs_rgb.z * -p.hs_b_),
                    (hs_rgb.y * -p.hs_g_) - (hs_rgb.x * p.hs_r_));
    rgb = odrt_add(rgb, hsf);
  }

  if (p.hs_cmy_enable_) {
    const float  tsn_pt_compl = 1.0f - tsn_pt;
    const float3 hs_cmy =
        make_float3(ha_cmy.x * ach_d * powf(tsn_pt_compl, 1.0f / p.hs_c_rng_),
                    ha_cmy.y * ach_d * powf(tsn_pt_compl, 1.0f / p.hs_m_rng_),
                    ha_cmy.z * ach_d * powf(tsn_pt_compl, 1.0f / p.hs_y_rng_));
    const float3 hsf =
        make_float3((hs_cmy.z * p.hs_y_) - (hs_cmy.y * p.hs_m_),
                    (hs_cmy.x * -p.hs_c_) - (hs_cmy.z * p.hs_y_),
                    (hs_cmy.y * p.hs_m_) - (hs_cmy.x * -p.hs_c_));
    rgb = odrt_add(rgb, hsf);
  }

  const float pt_lml_p =
      1.0f + 4.0f * (1.0f - tsn_pt) *
                 (p.pt_lml_ + p.pt_lml_r_ * ha_rgb_hs.x + p.pt_lml_g_ * ha_rgb_hs.y +
                  p.pt_lml_b_ * ha_rgb_hs.z);
  float ptf = 1.0f - powf(tsn_pt, pt_lml_p);
  const float pt_lmh_p =
      (1.0f - ach_d * (p.pt_lmh_r_ * ha_rgb_hs.x + p.pt_lmh_b_ * ha_rgb_hs.z)) *
      (1.0f - p.pt_lmh_ * ach_d);
  ptf = powf(ptf, pt_lmh_p);

  if (p.ptm_enable_) {
    float ptm_low_f = 1.0f;
    if (p.ptm_low_st_ != 0.0f && p.ptm_low_rng_ != 0.0f) {
      ptm_low_f = 1.0f + p.ptm_low_ * expf(-2.0f * ach_d * ach_d / p.ptm_low_st_) *
                             powf(1.0f - tsn_const, 1.0f / p.ptm_low_rng_);
    }
    float ptm_high_f = 1.0f;
    if (p.ptm_high_st_ != 0.0f && p.ptm_high_rng_ != 0.0f) {
      ptm_high_f = 1.0f + p.ptm_high_ * expf(-2.0f * ach_d * ach_d / p.ptm_high_st_) *
                              powf(tsn_pt, 1.0f / (4.0f * p.ptm_high_rng_));
    }
    ptf *= ptm_low_f * ptm_high_f;
  }

  rgb = odrt_add(odrt_mul_scalar(rgb, ptf), make_float3(1.0f - ptf, 1.0f - ptf, 1.0f - ptf));

  sat_L = rgb.x * rs_w.x + rgb.y * rs_w.y + rgb.z * rs_w.z;
  rgb   = odrt_div_scalar(
      odrt_sub(odrt_mul_scalar(make_float3(sat_L, sat_L, sat_L), p.rs_sa_), rgb),
      p.rs_sa_ - 1.0f);

  rgb = odrt_display_gamut_whitepoint(rgb, tsn_const, p.cwp_lm_, p.display_gamut_,
                                      p.creative_white_);

  if (p.brlp_enable_) {
    const float2 brlp_opp   = odrt_opponent(rgb);
    float        brlp_ach_d = odrt_hypot2(brlp_opp) / 4.0f;
    brlp_ach_d             = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
    const float3 brlp_ha_rgb = odrt_mul_scalar(ha_rgb, ach_d);
    const float  brlp_m = p.brlp_ + p.brlp_r_ * brlp_ha_rgb.x + p.brlp_g_ * brlp_ha_rgb.y +
                          p.brlp_b_ * brlp_ha_rgb.z;
    rgb = odrt_mul_scalar(rgb, powf(2.0f, brlp_m * brlp_ach_d * tsn));
  }

  if (p.ptl_enable_) {
    rgb = make_float3(odrt_softplus(rgb.x, p.ptl_c_), odrt_softplus(rgb.y, p.ptl_m_),
                      odrt_softplus(rgb.z, p.ptl_y_));
  }

  tsn *= p.ts_m2_;
  tsn  = odrt_compress_toe_quadratic(tsn, p.tn_toe_, 0);
  tsn *= p.ts_dsc_;
  rgb  = odrt_mul_scalar(rgb, tsn);

  if (p.display_gamut_ == 2) {
    rgb = odrt_clampminf3(rgb, 0.0f);
    rgb = odrt_apply_matrix(OPEN_DRT_P3_TO_REC2020, rgb);
  }

  if (p.clamp_) {
    rgb = odrt_clampf3(rgb, 0.0f, 1.0f);
  }

  return rgb;
}

}  // namespace alcedo::CUDA
