//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <opencv2/core.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {
namespace ColorUtils {

enum class ColorSpace : int {
  AP0,
  AP1,
  REC709,
  REC2020,
  P3_D65,
  P3_D60,
  P3_DCI,
  XYZ,
  PROPHOTO,
  ADOBE_RGB,
};

enum class ODTMethod : int {
  ACES_2_0 = 0,
  OPEN_DRT = 1,
};

enum class EOTF : int {
  LINEAR    = 0,
  ST2084    = 1,
  HLG       = 2,
  GAMMA_2_6 = 3,
  BT1886    = 4,
  GAMMA_2_2 = 5,
  GAMMA_1_8 = 6,
};

struct ColorSpacePrimaries {
  float red_[2];
  float green_[2];
  float blue_[2];
  float white_[2];
};

const ColorSpacePrimaries AP0_PRIMARY = {
    {0.73470f, 0.26530f}, {0.00000f, 1.00000f}, {0.00010f, -0.07700f}, {0.32168f, 0.33767f}};

const ColorSpacePrimaries AP1_PRIMARY = {
    {0.713f, 0.293f}, {0.165f, 0.830f}, {0.128f, 0.044f}, {0.32168f, 0.33767f}};

const ColorSpacePrimaries REACH_PRIMARY = {
    {0.713f, 0.293f}, {0.165f, 0.830f}, {0.128f, 0.044f}, {0.32168f, 0.33767f}};

const ColorSpacePrimaries REC709_PRIMARY = {
    {0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, {0.3127f, 0.3290f}};

const ColorSpacePrimaries REC2020_PRIMARY = {
    {0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, {0.3127f, 0.3290f}};

const ColorSpacePrimaries P3_D65_PRIMARY = {
    {0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, {0.3127f, 0.3290f}};

const ColorSpacePrimaries P3_D60_PRIMARY = {
    {0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, {0.32168f, 0.33767f}};

const ColorSpacePrimaries P3_DCI_PRIMARY = {
    {0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, {0.314f, 0.351f}};

const ColorSpacePrimaries PROPHOTO_PRIMARY = {
    {0.734699f, 0.265301f}, {0.159597f, 0.840403f}, {0.036598f, 0.000105f}, {0.345704f, 0.358540f}};

const ColorSpacePrimaries ADOBE_RGB_PRIMARY = {
    {0.6400f, 0.3300f}, {0.2100f, 0.7100f}, {0.1500f, 0.0600f}, {0.3127f, 0.3290f}};

inline ColorSpacePrimaries SpaceEnumToPrimary(ColorSpace cs) {
  switch (cs) {
    case ColorSpace::AP0:
      return AP0_PRIMARY;
    case ColorSpace::AP1:
      return AP1_PRIMARY;
    case ColorSpace::REC709:
      return REC709_PRIMARY;
    case ColorSpace::REC2020:
      return REC2020_PRIMARY;
    case ColorSpace::P3_D65:
      return P3_D65_PRIMARY;
    case ColorSpace::P3_D60:
      return P3_D60_PRIMARY;
    case ColorSpace::P3_DCI:
      return P3_DCI_PRIMARY;
    case ColorSpace::PROPHOTO:
      return PROPHOTO_PRIMARY;
    case ColorSpace::ADOBE_RGB:
      return ADOBE_RGB_PRIMARY;
    case ColorSpace::XYZ:
      return REC709_PRIMARY;
    default:
      return REC709_PRIMARY;
  }
}

inline auto ColorSpaceFromString(std::string_view cs_str) -> ColorSpace {
  if (cs_str == "ap0") {
    return ColorSpace::AP0;
  }
  if (cs_str == "ap1") {
    return ColorSpace::AP1;
  }
  if (cs_str == "rec709") {
    return ColorSpace::REC709;
  }
  if (cs_str == "rec2020") {
    return ColorSpace::REC2020;
  }
  if (cs_str == "p3_d65") {
    return ColorSpace::P3_D65;
  }
  if (cs_str == "p3_d60") {
    return ColorSpace::P3_D60;
  }
  if (cs_str == "p3_dci") {
    return ColorSpace::P3_DCI;
  }
  if (cs_str == "xyz") {
    return ColorSpace::XYZ;
  }
  if (cs_str == "prophoto") {
    return ColorSpace::PROPHOTO;
  }
  if (cs_str == "adobe_rgb") {
    return ColorSpace::ADOBE_RGB;
  }
  return ColorSpace::REC709;
}

inline auto ColorSpaceToString(ColorSpace cs) -> std::string {
  switch (cs) {
    case ColorSpace::AP0:
      return "ap0";
    case ColorSpace::AP1:
      return "ap1";
    case ColorSpace::REC709:
      return "rec709";
    case ColorSpace::REC2020:
      return "rec2020";
    case ColorSpace::P3_D65:
      return "p3_d65";
    case ColorSpace::P3_D60:
      return "p3_d60";
    case ColorSpace::P3_DCI:
      return "p3_dci";
    case ColorSpace::XYZ:
      return "xyz";
    case ColorSpace::PROPHOTO:
      return "prophoto";
    case ColorSpace::ADOBE_RGB:
      return "adobe_rgb";
    default:
      return "rec709";
  }
}

inline auto ODTMethodFromString(std::string_view method_str) -> ODTMethod {
  if (method_str == "aces_2_0") {
    return ODTMethod::ACES_2_0;
  }
  return ODTMethod::OPEN_DRT;
}

inline auto ODTMethodToString(ODTMethod method) -> std::string {
  switch (method) {
    case ODTMethod::ACES_2_0:
      return "aces_2_0";
    case ODTMethod::OPEN_DRT:
      return "open_drt";
    default:
      return "open_drt";
  }
}

inline cv::Matx33f RGB_TO_XYZ_f33(const ColorSpacePrimaries& C, float Y = 1.0f) {
  // X and Z values of RGB value (1, 1, 1), or "white"
  float X = C.white_[0] * Y / C.white_[1];
  float Z = (1.f - C.white_[0] - C.white_[1]) * Y / C.white_[1];

  // Scale factors for matrix rows
  float d = C.red_[0] * (C.blue_[1] - C.green_[1]) + C.blue_[0] * (C.green_[1] - C.red_[1]) +
            C.green_[0] * (C.red_[1] - C.blue_[1]);

  float Sr = (X * (C.blue_[1] - C.green_[1]) -
              C.green_[0] * (Y * (C.blue_[1] - 1) + C.blue_[1] * (X + Z)) +
              C.blue_[0] * (Y * (C.green_[1] - 1) + C.green_[1] * (X + Z))) /
             d;

  float Sg =
      (X * (C.red_[1] - C.blue_[1]) + C.red_[0] * (Y * (C.blue_[1] - 1) + C.blue_[1] * (X + Z)) -
       C.blue_[0] * (Y * (C.red_[1] - 1) + C.red_[1] * (X + Z))) /
      d;

  float Sb =
      (X * (C.green_[1] - C.red_[1]) - C.red_[0] * (Y * (C.green_[1] - 1) + C.green_[1] * (X + Z)) +
       C.green_[0] * (Y * (C.red_[1] - 1) + C.red_[1] * (X + Z))) /
      d;
  cv::Matx33f M(Sr * C.red_[0], Sr * C.red_[1], Sr * (1.f - C.red_[0] - C.red_[1]),
                Sg * C.green_[0], Sg * C.green_[1], Sg * (1.f - C.green_[0] - C.green_[1]),
                Sb * C.blue_[0], Sb * C.blue_[1], Sb * (1.f - C.blue_[0] - C.blue_[1]));
  return M;
}

inline cv::Matx33f XYZ_TO_RGB_f33(const ColorSpacePrimaries& C, float Y = 1.0f) {
  cv::Matx33f M = RGB_TO_XYZ_f33(C, Y);
  return M.inv();
}

inline cv::Matx33f RGB_TO_XYZ_f33(ColorSpace cs, float Y = 1.0f) {
  if (cs == ColorSpace::XYZ) {
    return cv::Matx33f::eye();
  }
  return RGB_TO_XYZ_f33(SpaceEnumToPrimary(cs), Y);
}

inline cv::Matx33f XYZ_TO_RGB_f33(ColorSpace cs, float Y = 1.0f) {
  if (cs == ColorSpace::XYZ) {
    return cv::Matx33f::eye();
  }
  return XYZ_TO_RGB_f33(SpaceEnumToPrimary(cs), Y);
}

inline auto EOTFFromString(std::string_view eotf_str) -> EOTF {
  if (eotf_str == "linear") {
    return EOTF::LINEAR;
  }
  if (eotf_str == "st2084") {
    return EOTF::ST2084;
  }
  if (eotf_str == "hlg") {
    return EOTF::HLG;
  }
  if (eotf_str == "gamma_2_6") {
    return EOTF::GAMMA_2_6;
  }
  if (eotf_str == "bt1886") {
    return EOTF::BT1886;
  }
  if (eotf_str == "gamma_2_2") {
    return EOTF::GAMMA_2_2;
  }
  if (eotf_str == "gamma_1_8") {
    return EOTF::GAMMA_1_8;
  }
  return EOTF::GAMMA_2_2;
}

inline auto EOTFToString(EOTF eotf) -> std::string {
  switch (eotf) {
    case EOTF::LINEAR:
      return "linear";
    case EOTF::ST2084:
      return "st2084";
    case EOTF::HLG:
      return "hlg";
    case EOTF::GAMMA_2_6:
      return "gamma_2_6";
    case EOTF::BT1886:
      return "bt1886";
    case EOTF::GAMMA_2_2:
      return "gamma_2_2";
    case EOTF::GAMMA_1_8:
      return "gamma_1_8";
    default:
      return "gamma_2_2";
  }
}

// Chroma compression
const float chroma_compress        = 2.4f;
const float chroma_compress_fact   = 3.3f;
const float chroma_expand          = 1.3f;
const float chroma_expand_fact     = 0.69f;
const float chroma_expand_thr      = 0.5f;

// Gamut compression constants
const float smooth_cusps           = 0.12f;
const float smooth_J               = 0.0f;  // could be eliminated
const float smooth_M               = 0.27f;
const float cusp_mid_blend         = 1.3f;

const float focus_gain_blend       = 0.3f;
const float focus_adjust_gain      = 0.55f;
const float focus_distance         = 1.35f;
const float focus_distance_scaling = 1.75f;

// CAM Parameters
const float ref_lum                = 100.f;
const float L_A                    = 100.f;
const float Y_b                    = 20.f;

const float ac_resp                = 1.f;
const float ra                     = 2.f;
const float ba                     = 0.05f + (2.f - ra);

const float surround[3]            = {0.9f, 0.59f, 0.9f};  // Average viewing condition

const float J_scale                = 100.0f;
const float cam_nl_Y_reference     = 100.0f;
const float cam_nl_offset          = 0.2713f * cam_nl_Y_reference;
const float cam_nl_scale           = 4.0f * cam_nl_Y_reference;

const float model_gamma            = surround[1] * (1.48f + sqrtf(Y_b / ref_lum));

// Table generation
#define TOTAL_TABLE_SIZE 362
#define TABLE_SIZE       360

const int                 cusp_corner_count      = 6;
const int                 total_corner_count     = cusp_corner_count + 2;
const int                 max_sorted_corners     = 2 * cusp_corner_count;
const float               reach_cusp_tolerance   = 1e-3f;
const float               display_cusp_tolerance = 1e-7f;

const float               hue_limit              = 360.f;

const float               gamma_minimum          = 0.0f;
const float               gamma_maximum          = 5.0f;
const float               gamma_search_step      = 0.4f;
const float               gamma_accuracy         = 1e-5f;

const ColorSpacePrimaries CAM16_PRI              = {
    {0.8336f, 0.1735f}, {2.3854f, -1.4659f}, {0.087f, -0.125f}, {0.333f, 0.333f}};

const cv::Matx33f CAM16_RGB_TO_XYZ          = RGB_TO_XYZ_f33(CAM16_PRI, 1.f);
const cv::Matx33f MATRIX_16                 = XYZ_TO_RGB_f33(CAM16_PRI, 1.f);

const cv::Matx33f base_cone_repponse_to_Aab = {
    2.f, 1.f, 1.f / 9.f, 1.f, -12.f / 11.f, 1.f / 9.f, 1.f / 20.f, 1.f / 11.f, -2.f / 9.f};

inline cv::Matx33f GeneratePanlrcm(float _ra = 2.f, float _ba = 0.05f) {
  cv::Matx33f panlrcm_data = {_ra,       1.f, 1.f / 9.f,  1.f,       -12.f / 11.f,
                              1.f / 9.f, _ba, 1.f / 11.f, -2.f / 9.f};
  cv::Matx33f panlrcm      = panlrcm_data.inv();

  for (int i = 0; i < 3; ++i) {
    float n       = 460.f / panlrcm(0, i);
    panlrcm(0, i) = panlrcm(0, i) * n;
    panlrcm(1, i) = panlrcm(1, i) * n;
    panlrcm(2, i) = panlrcm(2, i) * n;
  }

  return panlrcm;
}

// CAM Functions
inline float _pacrc_fwd_(float Rc) {
  const float F_L_Y = powf(Rc, 0.42f);
  return (F_L_Y) / (cam_nl_offset + F_L_Y);
}

inline float pacrc_fwd(float v) {
  const float abs_v = fabsf(v);
  return copysignf(_pacrc_fwd_(abs_v), v);
}

inline float _pacrc_inv(float Ra) {
  const float Ra_lim = fminf(Ra, 0.99f);
  const float F_L_Y  = (cam_nl_offset * Ra_lim) / (1.f - Ra_lim);
  return powf(F_L_Y, 1.f / 0.42f);
}

inline float pacrc_inv(float v) {
  const float abs_v = fabsf(v);
  return copysignf(_pacrc_inv(abs_v), v);
}

const cv::Matx33f PANLRCM = GeneratePanlrcm(ra, ba);

struct JMhParams {
  cv::Matx33f MATRIX_RGB_to_CAM16_c_;
  cv::Matx33f MATRIX_CAM16_c_to_RGB_;
  cv::Matx33f MATRIX_cone_response_to_Aab_;
  cv::Matx33f MATRIX_Aab_to_cone_response_;
  float       F_L_n_;
  float       cz_;
  float       inv_cz_;
  float       A_w_z_;
  float       inv_A_w_J_;
};

struct TSParams {
  float n_;
  float n_r_;
  float g_;
  float t_1_;
  float c_t_;
  float s_2_;
  float u_2_;
  float m_2_;
  float forward_limit_;
  float inverse_limit_;
  float log_peak_;
};

struct ODTParams {
  float                                                      peak_luminance_;

  // JMh parameters
  JMhParams                                                  input_params_;
  JMhParams                                                  reach_params_;
  JMhParams                                                  limit_params_;

  // Tone scale, set via TSParams structure
  TSParams                                                   ts_params_;

  // Shared compression parameters
  float                                                      limit_J_max_;
  float                                                      model_gamma_inv_;
  std::shared_ptr<std::array<float, TOTAL_TABLE_SIZE>>       table_reach_M_;

  // Gamut compression parameters
  float                                                      mid_J_;
  float                                                      model_gamma_;
  float                                                      lower_hull_gamma_;
  float                                                      focus_dist_;
  float                                                      lower_hull_gamma_inv_;
  std::shared_ptr<std::array<float, TOTAL_TABLE_SIZE>>       table_hues_;
  std::shared_ptr<std::array<cv::Matx13f, TOTAL_TABLE_SIZE>> table_gamut_cusps_;
  std::shared_ptr<std::array<float, TOTAL_TABLE_SIZE>>       table_upper_hull_gammas_;
  cv::Matx12f                                                hue_linearity_search_range_;

  // Chroma compression parameters
  float                                                      sat_;
  float                                                      sat_thr_;
  float                                                      compr_;
  float                                                      chroma_compress_scale_;
};

struct OpenDRTParams {
  int   tn_hcon_enable_ = 0;
  int   tn_lcon_enable_ = 0;
  int   pt_enable_      = 1;
  int   ptl_enable_     = 1;
  int   ptm_enable_     = 1;
  int   brl_enable_     = 1;
  int   brlp_enable_    = 1;
  int   hc_enable_      = 1;
  int   hs_rgb_enable_  = 1;
  int   hs_cmy_enable_  = 1;
  int   creative_white_ = 2;
  int   surround_       = 2;
  int   clamp_          = 1;
  int   display_gamut_  = 0;
  int   display_eotf_   = 1;

  float tn_con_         = 1.66f;
  float tn_sh_          = 0.5f;
  float tn_toe_         = 0.003f;
  float tn_off_         = 0.005f;
  float tn_hcon_        = 0.0f;
  float tn_hcon_pv_     = 1.0f;
  float tn_hcon_st_     = 4.0f;
  float tn_lcon_        = 0.0f;
  float tn_lcon_w_      = 0.5f;
  float cwp_lm_         = 0.25f;
  float rs_sa_          = 0.35f;
  float rs_rw_          = 0.25f;
  float rs_bw_          = 0.55f;
  float pt_lml_         = 0.25f;
  float pt_lml_r_       = 0.5f;
  float pt_lml_g_       = 0.0f;
  float pt_lml_b_       = 0.1f;
  float pt_lmh_         = 0.25f;
  float pt_lmh_r_       = 0.5f;
  float pt_lmh_b_       = 0.0f;
  float ptl_c_          = 0.06f;
  float ptl_m_          = 0.08f;
  float ptl_y_          = 0.06f;
  float ptm_low_        = 0.4f;
  float ptm_low_rng_    = 0.25f;
  float ptm_low_st_     = 0.5f;
  float ptm_high_       = -0.8f;
  float ptm_high_rng_   = 0.35f;
  float ptm_high_st_    = 0.4f;
  float brl_            = 0.0f;
  float brl_r_          = -2.5f;
  float brl_g_          = -1.5f;
  float brl_b_          = -1.5f;
  float brl_rng_        = 0.5f;
  float brl_st_         = 0.35f;
  float brlp_           = -0.5f;
  float brlp_r_         = -1.25f;
  float brlp_g_         = -1.25f;
  float brlp_b_         = -0.25f;
  float hc_r_           = 1.0f;
  float hc_r_rng_       = 0.3f;
  float hs_r_           = 0.6f;
  float hs_r_rng_       = 0.6f;
  float hs_g_           = 0.35f;
  float hs_g_rng_       = 1.0f;
  float hs_b_           = 0.66f;
  float hs_b_rng_       = 1.0f;
  float hs_c_           = 0.25f;
  float hs_c_rng_       = 1.0f;
  float hs_m_           = 0.0f;
  float hs_m_rng_       = 1.0f;
  float hs_y_           = 0.0f;
  float hs_y_rng_       = 1.0f;

  float ts_x1_          = 0.0f;
  float ts_y1_          = 0.0f;
  float ts_x0_          = 0.0f;
  float ts_y0_          = 0.0f;
  float ts_s0_          = 0.0f;
  float ts_p_           = 0.0f;
  float ts_s10_         = 0.0f;
  float ts_m1_          = 0.0f;
  float ts_m2_          = 0.0f;
  float ts_s_           = 0.0f;
  float ts_dsc_         = 0.0f;
  float pt_cmp_Lf_      = 0.0f;
  float s_Lp100_        = 0.0f;
  float ts_s1_          = 0.0f;
};

struct TO_OUTPUT_Params {
  ODTMethod        method_               = ODTMethod::OPEN_DRT;
  ColorSpace       encoding_space_       = ColorSpace::REC709;
  EOTF             eotf_                 = EOTF::GAMMA_2_2;
  float            peak_luminance_       = 100.0f;
  float            display_linear_scale_ = 1.0f;
  ODTParams        aces_params_          = {};
  OpenDRTParams    open_drt_params_      = {};
  cv::Matx33f      limit_to_display_matx_ = cv::Matx33f::eye();
};

inline float signum(float x) {
  if (x > 0.f) {
    return 1.f;
  } else if (x < 0.f) {
    return -1.f;
  } else {
    return 0.f;
  }
}

inline cv::Matx13f mult_f3_f33(const cv::Matx13f& v, const cv::Matx33f& m) {
  return cv::Matx13f(v(0) * m(0, 0) + v(1) * m(1, 0) + v(2) * m(2, 0),
                     v(0) * m(0, 1) + v(1) * m(1, 1) + v(2) * m(2, 1),
                     v(0) * m(0, 2) + v(1) * m(1, 2) + v(2) * m(2, 2));
}

inline float Y_to_Hellwig_J(float Y, float _surround = 0.59f, float _L_A = 100.f,
                            float _Y_b = 20.f) {
  float k  = 1.f / (5.f * _L_A + 1.f);
  float k4 = k * k * k * k;
  float F_L =
      0.2f * k4 * (5.f * _L_A) + 0.1f * powf((1.f - k4), 2.f) * powf((5.f * _L_A), 1.f / 3.f);
  float n     = _Y_b / 100.f;
  float z     = 1.48f + sqrtf(n);
  float F_L_W = powf(F_L, 0.42f);
  float A_w   = (400.f * F_L_W) / (F_L_W + 27.13f);

  float F_L_Y = powf(F_L * fabsf(Y) / 100.f, 0.42f);

  return signum(Y) * 100.f * powf(((400.f * F_L_Y) / (27.13f + F_L_Y)) / A_w, _surround * z);
}

inline float Achromatic_n_to_J(float A, float cz) {
  return J_scale * powf(fmaxf(A, 0.f), cz);
}

inline float Y_to_J(float Y, JMhParams& p) {
  float abs_Y = fabsf(Y);
  float Ra    = _pacrc_fwd_(abs_Y * p.F_L_n_);
  float J     = Achromatic_n_to_J(Ra * p.inv_A_w_J_, p.cz_);

  return copysignf(J, Y);
}

inline float wrap_to_360(float hue) {
  float y = fmodf(hue, 360.f);
  if (y < 0.f) {
    y += 360.f;
  }
  return y;
}

inline float J_to_Achromatic_n(float J, float inv_cz) {
  return powf(fmaxf(J, 0.f) * (1.f / J_scale), inv_cz);
}

inline cv::Matx13f JMh_to_Aab(cv::Matx13f& JMh, JMhParams& params) {
  float       J      = JMh(0);
  float       M      = JMh(1);
  float       h      = JMh(2);
  float       h_rad  = h * static_cast<float>(CV_PI) / 180.f;
  float       cos_hr = cosf(h_rad);
  float       sin_hr = sinf(h_rad);

  float       A      = J_to_Achromatic_n(J, params.inv_cz_);
  float       a      = M * cos_hr;
  float       b      = M * sin_hr;

  cv::Matx13f Aab    = cv::Matx13f(A, a, b);
  return Aab;
}

inline cv::Matx13f Aab_to_JMh(cv::Matx13f& Aab, JMhParams& params) {
  cv::Matx13f JMh = {0.f, 0.f, 0.f};
  if (Aab(0) <= 0.f) {
    return JMh;
  }

  float J     = Achromatic_n_to_J(Aab(0), params.cz_);
  float M     = sqrtf(Aab(1) * Aab(1) + Aab(2) * Aab(2));
  float h_rad = atan2f(Aab(2), Aab(1));
  float h     = wrap_to_360(h_rad * 180.f / static_cast<float>(CV_PI));

  JMh(0)      = J;
  JMh(1)      = M;
  JMh(2)      = h;

  return JMh;
}

inline cv::Matx13f Aab_to_RGB(cv::Matx13f& Aab, JMhParams& params) {
  cv::Matx13f rgb_a = Aab * params.MATRIX_Aab_to_cone_response_;
  cv::Matx13f rgb_m(pacrc_inv(rgb_a(0)), pacrc_inv(rgb_a(1)), pacrc_inv(rgb_a(2)));
  cv::Matx13f rgb = rgb_m * params.MATRIX_CAM16_c_to_RGB_;
  return rgb;
}

inline cv::Matx13f RGB_to_Aab(cv::Matx13f& RGB, JMhParams& params) {
  cv::Matx13f rgb_m = RGB * params.MATRIX_RGB_to_CAM16_c_;
  cv::Matx13f rgb_a(pacrc_fwd(rgb_m(0)), pacrc_fwd(rgb_m(1)), pacrc_fwd(rgb_m(2)));

  return rgb_a * params.MATRIX_cone_response_to_Aab_;
}

inline cv::Matx13f JMh_to_RGB(cv::Matx13f& JMh, JMhParams& params) {
  cv::Matx13f Aab = JMh_to_Aab(JMh, params);
  cv::Matx13f RGB = Aab_to_RGB(Aab, params);
  return RGB;
}

inline cv::Matx13f RGB_to_JMh(cv::Matx13f& RGB, JMhParams& params) {
  cv::Matx13f Aab = RGB_to_Aab(RGB, params);
  cv::Matx13f JMh = Aab_to_JMh(Aab, params);

  return JMh;
}

inline std::shared_ptr<std::array<float, TOTAL_TABLE_SIZE>> MakeReachMTable(JMhParams& params,
                                                                            float limit_J_max) {
  auto table_reach_M = std::make_shared<std::array<float, TOTAL_TABLE_SIZE>>();
  for (int i = 0; i < TABLE_SIZE; ++i) {
    float       hue            = (float)i * 360.f / TABLE_SIZE;

    const float search_range   = 50.f;
    const float search_maximum = 1300.f;
    float       low            = 0.f;
    float       high           = low + search_range;
    bool        outside        = false;

    while ((!outside) && (high < search_maximum)) {
      cv::Matx13f search_JMh = {limit_J_max, high, hue};
      cv::Matx13f search_RGB = JMh_to_RGB(search_JMh, params);
      outside = (search_RGB(0) < 0.f) || (search_RGB(1) < 0.f) || (search_RGB(2) < 0.f);
      if (!outside) {
        low = high;
        high += search_range;
      }
    }

    while (high - low > 1e-2f) {
      float       sample_M      = (high + low) * 0.5f;
      cv::Matx13f search_JMh    = {limit_J_max, sample_M, hue};
      cv::Matx13f new_limit_RGB = JMh_to_RGB(search_JMh, params);
      outside = (new_limit_RGB(0) < 0.f) || (new_limit_RGB(1) < 0.f) || (new_limit_RGB(2) < 0.f);
      if (outside) {
        high = sample_M;
      } else {
        low = sample_M;
      }
    }

    (*table_reach_M)[i + 1] = high;
  }

  (*table_reach_M)[0]              = (*table_reach_M)[TABLE_SIZE];

  (*table_reach_M)[1 + TABLE_SIZE] = (*table_reach_M)[1];

  return table_reach_M;
}

inline cv::Matx13f generate_unit_cube_cusp_corners(int corner) {
  cv::Matx13f result;
  // Generation order R, Y, G, C, B, M to ensure hues rotate in correct order
  if (((corner + 1) % cusp_corner_count) < 3)
    result(0) = 1;
  else
    result(0) = 0;
  if (((corner + 5) % cusp_corner_count) < 3)
    result(1) = 1;
  else
    result(1) = 0;
  if (((corner + 3) % cusp_corner_count) < 3)
    result(2) = 1;
  else
    result(2) = 0;
  return result;
}

inline void find_reach_corners_table(std::array<cv::Matx13f, total_corner_count>& jmh_corners,
                                     JMhParams& params_reach, ODTParams& p) {
  std::array<cv::Matx13f, total_corner_count> temp_JMh_corners;

  float limit_A   = J_to_Achromatic_n(p.limit_J_max_, params_reach.inv_cz_);

  int   min_index = 0;
  for (int i = 0; i < cusp_corner_count; ++i) {
    const cv::Matx13f rgb_vector = generate_unit_cube_cusp_corners(i);

    float             lower      = 0.f;
    float             upper      = p.ts_params_.forward_limit_;

    while ((upper - lower) > reach_cusp_tolerance) {
      float       test = (lower + upper) * 0.5f;
      cv::Matx13f test_corner =
          cv::Matx13f(rgb_vector(0) * test, rgb_vector(1) * test, rgb_vector(2) * test);
      float A = RGB_to_Aab(test_corner, params_reach)(0);
      if (A < limit_A) {
        lower = test;
      } else {
        upper = test;
      }
    }

    cv::Matx13f scale_rgb_vec =
        cv::Matx13f(rgb_vector(0) * upper, rgb_vector(1) * upper, rgb_vector(2) * upper);
    temp_JMh_corners[i] = RGB_to_JMh(scale_rgb_vec, params_reach);

    if (temp_JMh_corners[i](2) < temp_JMh_corners[min_index](2)) {
      min_index = i;
    }
  }

  // Rotate entries placing lowest at [1]
  for (int i = 0; i < cusp_corner_count; ++i) {
    jmh_corners[i + 1] = temp_JMh_corners[(i + min_index) % cusp_corner_count];
  }

  // Copy and elments to create a cycle
  jmh_corners[0]                     = jmh_corners[cusp_corner_count];
  jmh_corners[cusp_corner_count + 1] = jmh_corners[1];

  // Wrap the hues, to maintain monotonicity these entries will fall outside [0.0, hue_limit)
  jmh_corners[0](2) -= hue_limit;
  jmh_corners[cusp_corner_count + 1](2) += hue_limit;
}

inline std::array<float, max_sorted_corners> extract_sorted_cube_hues(
    std::array<cv::Matx13f, total_corner_count>& reach_JMh,
    std::array<cv::Matx13f, total_corner_count>& limit_JMh) {
  std::array<float, max_sorted_corners> sorted_hues{};

  // Basic merge of 2 sorted arrays, extracting the unique hues.
  // Return the count of the unique hues
  int                                   idx       = 0;
  int                                   reach_idx = 1;
  int                                   limit_idx = 1;

  while ((reach_idx <= cusp_corner_count) || (limit_idx <= cusp_corner_count)) {
    float reach_hue = reach_JMh[reach_idx](2);
    float limit_hue = limit_JMh[limit_idx](2);
    if (reach_hue == limit_hue) {
      sorted_hues[idx] = reach_hue;
      ++reach_idx;
      ++limit_idx;
    } else {
      if (reach_hue < limit_hue) {
        sorted_hues[idx] = reach_hue;
        ++reach_idx;
      } else {
        sorted_hues[idx] = limit_hue;
        ++limit_idx;
      }
    }
    ++idx;
  }
  return sorted_hues;
}

inline void build_limiting_cusp_tables(std::array<cv::Matx13f, total_corner_count>& RGB_corners,
                                       std::array<cv::Matx13f, total_corner_count>& JMh_corners,
                                       JMhParams& limit_params, ODTParams& p) {
  // We calculate the RGB and JMh values for the limiting gamut cusp corners
  // They are then arranged into a cycle with the lowest JMh value at [1] to
  // allow for hue wrapping
  std::array<cv::Matx13f, total_corner_count> temp_RGB_corners{};
  std::array<cv::Matx13f, total_corner_count> temp_JMh_corners{};

  int                                         min_index = 0;
  for (int i = 0; i < cusp_corner_count; ++i) {
    temp_RGB_corners[i] = {p.peak_luminance_ / ref_lum * generate_unit_cube_cusp_corners(i)(0),
                           p.peak_luminance_ / ref_lum * generate_unit_cube_cusp_corners(i)(1),
                           p.peak_luminance_ / ref_lum * generate_unit_cube_cusp_corners(i)(2)};
    temp_JMh_corners[i] = RGB_to_JMh(temp_RGB_corners[i], limit_params);
    if (temp_JMh_corners[i](2) < temp_JMh_corners[min_index](2)) {
      min_index = i;
    }
  }

  // Rotate entries placing lowest at [1]
  for (int i = 0; i < cusp_corner_count; ++i) {
    RGB_corners[i + 1] = temp_RGB_corners[(i + min_index) % cusp_corner_count];
    JMh_corners[i + 1] = temp_JMh_corners[(i + min_index) % cusp_corner_count];
  }

  // Copy and elements to create a cycle
  RGB_corners[0]                     = RGB_corners[cusp_corner_count];
  RGB_corners[cusp_corner_count + 1] = RGB_corners[1];
  JMh_corners[0]                     = JMh_corners[cusp_corner_count];
  JMh_corners[cusp_corner_count + 1] = JMh_corners[1];

  // Wrap the hues, to maintain monotonicity these entries will fall outside [0.0, hue_limit)
  JMh_corners[0](2) -= hue_limit;
  JMh_corners[cusp_corner_count + 1](2) += hue_limit;
}

inline void build_hue_sample_interval(int samples, float lower, float upper,
                                      std::array<float, TOTAL_TABLE_SIZE>& hue_table, int base) {
  float delta = (upper - lower) / static_cast<float>(samples);
  int   i;
  for (i = 0; i < samples; ++i) {
    hue_table[base + i] = lower + delta * static_cast<float>(i);
  }
}

inline std::array<float, TOTAL_TABLE_SIZE> build_hue_table(
    std::array<float, max_sorted_corners>& sorted_hues) {
  std::array<float, TOTAL_TABLE_SIZE> hue_table{};

  float                               ideal_spacing = TABLE_SIZE / hue_limit;
  int                                 samples_count[2 * cusp_corner_count + 2] = {0};
  int                                 last_idx                                 = 0;
  int                                 min_index                                = 0;
  if (sorted_hues[0] == 0.f) {
    min_index = 0;
  } else {
    min_index = 1;
  }
  int hue_idx;

  for (hue_idx = 0; hue_idx != max_sorted_corners; ++hue_idx) {
    int nominal_idx = static_cast<int>(
        fminf(fmaxf(roundf((sorted_hues[hue_idx] * ideal_spacing)), static_cast<float>(min_index)),
              static_cast<float>(TABLE_SIZE - 1)));

    if (last_idx == nominal_idx) {
      // Last two hues should sample at same index, need to adjust them
      // Adjust previou sample down if we can
      if (hue_idx > 1 && samples_count[hue_idx - 2] != (samples_count[hue_idx - 1] - 1)) {
        samples_count[hue_idx - 1] -= 1;
      } else {
        nominal_idx += 1;
      }
    }
    samples_count[hue_idx] = static_cast<int>(
        fminf(static_cast<float>(nominal_idx), static_cast<float>(TABLE_SIZE - 1)));
    min_index = nominal_idx;
    last_idx  = min_index;
  }

  int total_samples = 0;
  // Special cases for ends
  int i             = 0;
  build_hue_sample_interval(samples_count[i], 0.0f, sorted_hues[i], hue_table, total_samples + 1);
  total_samples += samples_count[i];

  for (i = i + 1; i < max_sorted_corners; ++i) {
    int samples = samples_count[i] - samples_count[i - 1];
    build_hue_sample_interval(samples, sorted_hues[i - 1], sorted_hues[i], hue_table,
                              total_samples + 1);
    total_samples += samples;
  }

  build_hue_sample_interval(TABLE_SIZE - total_samples, sorted_hues[i - 1], hue_limit, hue_table,
                            total_samples + 1);

  hue_table[0]              = hue_table[1 + TABLE_SIZE - 1] - hue_limit;
  hue_table[TABLE_SIZE + 1] = hue_table[1] + hue_limit;
  return hue_table;
}

inline cv::Matx13f lerp_f3(const cv::Matx13f& a, const cv::Matx13f& b, float t) {
  return cv::Matx13f(std::lerp(a(0), b(0), t), std::lerp(a(1), b(1), t), std::lerp(a(2), b(2), t));
}

inline cv::Matx12f find_display_cusp_for_hue(
    float hue, std::array<cv::Matx13f, total_corner_count>& RGB_corners,
    std::array<cv::Matx13f, total_corner_count>& JMh_corners, JMhParams& params,
    cv::Matx12f& previous) {
  // This works by finding the required line segment between two of the XYZ
  // cusp corners, then binary searching along the line calculating the JMh of
  // points along the line till we find the required value. All values on the
  // line segments are valid cusp locations.
  cv::Matx12f return_JM;

  int         upper_corner = 1;
  int         found        = 0;
  for (int i = upper_corner; i != total_corner_count && !found; ++i) {
    if (hue < JMh_corners[i](2)) {
      upper_corner = i;
      found        = 1;
    }
  }
  int lower_corner = upper_corner - 1;

  // hue should now be within [lower_corner, upper_corner), handle exact match
  if (JMh_corners[lower_corner](2) == hue) {
    return_JM(0) = JMh_corners[lower_corner](0);
    return_JM(1) = JMh_corners[lower_corner](1);
    return return_JM;
  }

  // Search by lerping between RGB corners for the hue
  cv::Matx13f cusp_lower = RGB_corners[lower_corner];
  cv::Matx13f cusp_upper = RGB_corners[upper_corner];
  cv::Matx13f sample;

  float       sample_t;
  float       lower_t = 0.f;
  if (upper_corner == previous(0)) lower_t = previous(1);
  float       upper_t = 1.f;

  cv::Matx13f JMh;

  // There is an edge case where we need to search towards the range when
  // across the [0.0, hue_limit] boundary each edge needs the directions
  // swapped. This is handled by comparing against the appropriate corner to
  // make sure we are still in the expected range between the lower and upper
  // corner hue limits
  while ((upper_t - lower_t) > display_cusp_tolerance) {
    sample_t = std::midpoint(lower_t, upper_t);
    sample   = lerp_f3(cusp_lower, cusp_upper, sample_t);
    JMh      = RGB_to_JMh(sample, params);
    if (JMh(2) < JMh_corners[lower_corner](2)) {
      upper_t = sample_t;
    } else if (JMh(2) >= JMh_corners[upper_corner](2)) {
      lower_t = sample_t;
    } else if (JMh(2) > hue) {
      upper_t = sample_t;
    } else {
      lower_t = sample_t;
    }
  }

  sample_t     = std::midpoint(lower_t, upper_t);
  sample       = lerp_f3(cusp_lower, cusp_upper, sample_t);
  JMh          = RGB_to_JMh(sample, params);
  return_JM(0) = JMh(0);
  return_JM(1) = JMh(1);
  return return_JM;
}

inline std::array<cv::Matx13f, TOTAL_TABLE_SIZE> build_cusp_table(
    std::array<float, TOTAL_TABLE_SIZE>&         hue_table,
    std::array<cv::Matx13f, total_corner_count>& RGB_corners,
    std::array<cv::Matx13f, total_corner_count>& JMh_corners, JMhParams& params) {
  cv::Matx12f                               previous = {0.f, 0.f};
  std::array<cv::Matx13f, TOTAL_TABLE_SIZE> output_table{};

  for (int i = 1; i < TOTAL_TABLE_SIZE; ++i) {
    float hue          = hue_table[i];
    auto  JM           = find_display_cusp_for_hue(hue, RGB_corners, JMh_corners, params, previous);
    output_table[i](0) = JM(0);
    output_table[i](1) = JM(1) * (1.f + smooth_M * smooth_cusps);
    output_table[i](2) = hue;
  }

  // Copy last nominal entry to start
  output_table[0](0)              = output_table[TABLE_SIZE](0);
  output_table[0](1)              = output_table[TABLE_SIZE](1);
  output_table[0](2)              = hue_table[0];

  // Copy first nominal entry to end
  output_table[TABLE_SIZE + 1](0) = output_table[1](0);
  output_table[TABLE_SIZE + 1](1) = output_table[1](1);
  output_table[TABLE_SIZE + 1](2) = hue_table[TABLE_SIZE + 1];
  return output_table;
}

const int   test_count                = 5;
const float testPositions[test_count] = {0.01f, 0.1f, 0.5f, 0.8f, 0.99f};

struct TestData {
  std::array<float, 3> test_JMh_;
  float                J_intersect_source_;
  float                slope_;
  float                J_intersect_cusp_;
};

inline float compute_focus_J(float cusp_J, float mid_J, float limit_J_max) {
  return std::lerp(cusp_J, mid_J, fminf(1.f, cusp_mid_blend - (cusp_J / limit_J_max)));
}

inline float get_focus_gain(float J, float analytical_threshold, float limit_J_Max,
                            float focus_dist) {
  float gain = limit_J_Max * focus_dist;

  if (J > analytical_threshold) {
    float gain_adjustment =
        log10f((limit_J_Max - analytical_threshold) / fmaxf(0.0001f, limit_J_Max - J));
    if (!std::isfinite(gain_adjustment)) {
      gain_adjustment = 0.f;
    }
    gain_adjustment = gain_adjustment * gain_adjustment + 1.f;
    gain            = gain * gain_adjustment;
  }
  return gain;
}

inline float solve_J_intersect(float J, float M, float focus_J, float max_J, float slope_gain) {
  const float sg       = fmaxf(fabsf(slope_gain), 1e-6f);
  const float fj       = fmaxf(fabsf(focus_J), 1e-6f);
  const float M_scaled = M / sg;
  const float a        = M_scaled / fj;

  if (J < focus_J) {
    const float b    = 1.f - M_scaled;
    const float c    = -J;
    const float det  = b * b - 4.f * a * c;
    const float root = (det > 0.f) ? sqrtf(det) : 0.f;
    const float den  = copysignf(fmaxf(fabsf(b + root), 1e-6f), b + root);
    return -2.f * c / den;
  }
  const float b    = -(1.f + M_scaled + max_J * a);
  const float c    = max_J * M_scaled + J;
  const float det  = b * b - 4.f * a * c;
  const float root = (det > 0.f) ? sqrtf(det) : 0.f;
  const float den  = copysignf(fmaxf(fabsf(b - root), 1e-6f), b - root);
  return -2.f * c / den;
}

inline float compute_compression_vector_slope(float intersect_J, float focus_J, float limit_J_max,
                                              float slope_gain) {
  float direction_scalar;
  if (intersect_J < focus_J) {
    direction_scalar = intersect_J;
  } else {
    direction_scalar = limit_J_max - intersect_J;
  }
  return direction_scalar * (intersect_J - focus_J) / (focus_J * slope_gain);
}

inline void generate_gamma_test_data(cv::Matx12f& JM_cusp, float hue, float limit_J_max,
                                     float mid_J, float focus_dist,
                                     std::array<cv::Matx13f, test_count>& test_JMh,
                                     std::array<float, test_count>&       J_intersect_source,
                                     std::array<float, test_count>&       slopes,
                                     std::array<float, test_count>&       J_intersect_cusp) {
  float analytical_threshold = std::lerp(JM_cusp(0), limit_J_max, focus_gain_blend);
  float focus_J              = compute_focus_J(JM_cusp(0), mid_J, limit_J_max);

  for (int test_idx = 0; test_idx != test_count; ++test_idx) {
    float test_J      = std::lerp(JM_cusp(0), limit_J_max, testPositions[test_idx]);
    float slope_gain  = get_focus_gain(test_J, analytical_threshold, limit_J_max, focus_dist);
    // float J_intersect =
    float J_intersect = solve_J_intersect(test_J, JM_cusp(1), focus_J, limit_J_max, slope_gain);
    float slope  = compute_compression_vector_slope(J_intersect, focus_J, limit_J_max, slope_gain);
    float J_cusp = solve_J_intersect(JM_cusp(0), JM_cusp(1), focus_J, limit_J_max, slope_gain);

    test_JMh[test_idx]           = cv::Matx13f(test_J, JM_cusp(1), hue);
    J_intersect_source[test_idx] = J_intersect;
    slopes[test_idx]             = slope;
    J_intersect_cusp[test_idx]   = J_cusp;
  }
}

inline float estimate_line_and_boundary_intersection_M(float J_axis_intersect, float slope,
                                                       float inv_gamma, float J_max, float M_max,
                                                       float J_intersection_reference) {
  // Line defined by     J = slope * x + J_axis_intersect
  // Boundary defined by J = J_max * (x / M_max) ^ (1/inv_gamma)
  // Approximate as we do not want to iteratively solve intersection of a
  // straight line and an exponential

  // We calculate a shifted intersection from the original intersection using
  // the inverse of the exponential and the provided reference
  const float refJ                 = fmaxf(J_intersection_reference, 1e-6f);
  const float normalized_J         = fmaxf(J_axis_intersect / refJ, 0.0f);
  const float shifted_intersection =
      (normalized_J == 0.0f) ? 0.0f : refJ * powf(normalized_J, inv_gamma);

  // Now we find the M intersection of two lines
  // line from origin to J,M Max       l1(x) = J/M * x
  // line from J Intersect' with slope l2(x) = slope * x + Intersect'

  // return shifted_intersection / ((J_max / M_max) - slope);
  const float denom = copysignf(fmaxf(fabsf(J_max - slope * M_max), 1e-6f), (J_max - slope * M_max));
  return shifted_intersection * M_max / denom;
}

inline float smin_scaled(float a, float b, float scale_ref) {
  const float s_scaled = smooth_cusps * scale_ref;
  if (s_scaled <= 1e-6f) return fminf(a, b);
  const float h        = fmaxf(s_scaled - fabsf(a - b), 0.f) / s_scaled;
  return fminf(a, b) - h * h * h * s_scaled * (1.f / 6.f);
}

inline float find_gamut_boundary_intersection(cv::Matx12f& JM_cusp, float J_max,
                                              float gamma_top_inv, float gamma_bottom_inv,
                                              float J_intersect_source, float slope,
                                              float J_intersect_cusp) {
  const float M_boundary_lower = estimate_line_and_boundary_intersection_M(
      J_intersect_source, slope, gamma_bottom_inv, JM_cusp(0), JM_cusp(1), J_intersect_cusp);
  // The upper hull is flipped and thus 'zeroed' at J_max
  // Also note we negate the slope
  const float f_J_intersect_cusp   = J_max - J_intersect_cusp;
  const float f_J_intersect_source = J_max - J_intersect_source;
  const float f_JM_cusp_J          = J_max - JM_cusp(0);
  const float M_boundary_upper     = estimate_line_and_boundary_intersection_M(
      f_J_intersect_source, -slope, gamma_top_inv, f_JM_cusp_J, JM_cusp(1), f_J_intersect_cusp);

  float M_boundary = smin_scaled(M_boundary_lower, M_boundary_upper, JM_cusp(1));
  return M_boundary;
}

inline bool outside_hull(cv::Matx13f& rgb, float max_rgb_test_val) {
  return rgb(0) > max_rgb_test_val || rgb(1) > max_rgb_test_val || rgb(2) > max_rgb_test_val;
}

inline bool evaluate_gamma_fit(cv::Matx12f& JM_cusp, std::array<cv::Matx13f, test_count>& test_JMh,
                               std::array<float, test_count>& J_intersect_source,
                               std::array<float, test_count>& slopes,
                               std::array<float, test_count>& J_intersect_cusp, float top_gamma_inv,
                               float peak_luminance, float limit_J_max, float lower_hull_gamma_inv,
                               JMhParams& limit_params) {
  float luminance_limit = peak_luminance / ref_lum;

  for (int test_idx = 0; test_idx < test_count; ++test_idx) {
    // Compute gamut boundary intersection
    float approxLimit_M = find_gamut_boundary_intersection(
        JM_cusp, limit_J_max, top_gamma_inv, lower_hull_gamma_inv, J_intersect_source[test_idx],
        slopes[test_idx], J_intersect_cusp[test_idx]);
    float       approxLimit_J   = J_intersect_source[test_idx] + slopes[test_idx] * approxLimit_M;

    // Store JMh values
    cv::Matx13f approximate_JMh = {approxLimit_J, approxLimit_M, test_JMh[test_idx](2)};

    // Convert to RGB
    cv::Matx13f new_limit_RGB   = JMh_to_RGB(approximate_JMh, limit_params);

    // Check if any values exceed the luminance limit. If so, we are outside of the top gamut shell
    if (!outside_hull(new_limit_RGB, luminance_limit)) return false;
  }

  return true;
}

inline std::array<float, TOTAL_TABLE_SIZE> MakeUpperHullGammaTable(
    std::array<cv::Matx13f, TOTAL_TABLE_SIZE>& gamut_cusp_table, ODTParams& p) {
  // Find upper hull gamma values for the gamut mapper.
  // Start by taking a h angle
  // Get the cusp J value for that angle
  // Find a J value halfway to the Jmax
  // Iterate through gamma values until the approximate max M is
  // negative through the actual boundary

  // positions between the cusp and Jmax we will check variables that get
  // set as we iterate through, once all are set to true we break the loop

  std::array<float, TOTAL_TABLE_SIZE> upper_hull_gammas{};

  for (int i = 1; i != 1 + TABLE_SIZE; ++i) {
    // Get cusp from cusp table at hue position
    float                               hue     = gamut_cusp_table[i](2);
    cv::Matx12f                         JM_cusp = {gamut_cusp_table[i](0), gamut_cusp_table[i](1)};

    std::array<cv::Matx13f, test_count> test_JMh{};
    std::array<float, test_count>       J_intersect_source{};
    std::array<float, test_count>       slopes{};
    std::array<float, test_count>       J_intersect_cusp{};

    generate_gamma_test_data(JM_cusp, hue, p.limit_J_max_, p.mid_J_, p.focus_dist_, test_JMh,
                             J_intersect_source, slopes, J_intersect_cusp);
    float search_range = gamma_search_step;
    float low          = gamma_minimum;
    float high         = low + search_range;
    bool  outside      = false;
    while (!(outside) && (high < gamma_maximum)) {
      bool gamma_found = evaluate_gamma_fit(
          JM_cusp, test_JMh, J_intersect_source, slopes, J_intersect_cusp, 1.f / high,
          p.peak_luminance_, p.limit_J_max_, p.lower_hull_gamma_inv_, p.limit_params_);
      if (!gamma_found) {
        low = high;
        high += search_range;
      } else {
        outside = true;
      }
    }

    float test_gamma = -1.f;
    while ((high - low) > gamma_accuracy) {
      test_gamma       = std::midpoint(high, low);
      bool gamma_found = evaluate_gamma_fit(
          JM_cusp, test_JMh, J_intersect_source, slopes, J_intersect_cusp, 1.f / test_gamma,
          p.peak_luminance_, p.limit_J_max_, p.lower_hull_gamma_inv_, p.limit_params_);
      if (gamma_found) {
        high = test_gamma;
      } else {
        low = test_gamma;
      }
    }
    upper_hull_gammas[i] = 1.f / high;
    if (!std::isfinite(upper_hull_gammas[i]) || upper_hull_gammas[i] <= 0.f) {
      upper_hull_gammas[i] = 1.f / fmaxf(high, gamma_search_step);
    }
  }

  // Copy last populated entry to first empty spot
  upper_hull_gammas[0]              = upper_hull_gammas[TABLE_SIZE];

  // Copy first populated entry to last empty spot
  upper_hull_gammas[TABLE_SIZE + 1] = upper_hull_gammas[1];

  return upper_hull_gammas;
}

inline std::shared_ptr<std::array<cv::Matx13f, TOTAL_TABLE_SIZE>> MakeUniformHueGamutTable(
    JMhParams& reach_params, JMhParams& limit_params, ODTParams& p) {
  std::array<cv::Matx13f, total_corner_count> reach_JMh_corners{};
  std::array<cv::Matx13f, total_corner_count> limiting_RGB_corners{};
  std::array<cv::Matx13f, total_corner_count> limiting_JMh_corners{};

  find_reach_corners_table(reach_JMh_corners, reach_params, p);
  build_limiting_cusp_tables(limiting_RGB_corners, limiting_JMh_corners, limit_params, p);
  auto sorted_hues = extract_sorted_cube_hues(reach_JMh_corners, limiting_JMh_corners);
  auto hue_table   = build_hue_table(sorted_hues);

  auto cusp_table  = std::make_shared<std::array<cv::Matx13f, TOTAL_TABLE_SIZE>>(
      build_cusp_table(hue_table, limiting_RGB_corners, limiting_JMh_corners, limit_params));
  return cusp_table;
}

inline int hue_position_in_uniform_table(float hue, int table_size) {
  const float wrapped_hue = wrap_to_360(hue);
  int         result      = static_cast<int>(wrapped_hue / hue_limit * table_size);
  return result;
}

inline cv::Matx12f DetermineHueLinearitySearchRange(
    std::array<float, TOTAL_TABLE_SIZE>& hue_table) {
  // This function searches through the hues looking for the largest
  // deviations from a linear distribution. We can then use this to initialise
  // the binary search range to something smaller than the full one to reduce
  // the number of lookups per hue lookup from ~ceil(log2(table size)) to
  // ~ceil(log2(range)) during image rendering.

  const int   lower_padding              = 0;
  const int   upper_padding              = 1;

  cv::Matx12f hue_linearity_search_range = {lower_padding, upper_padding};

  for (int i = 1; i != 1 + TABLE_SIZE; ++i) {
    const int pos   = hue_position_in_uniform_table(hue_table[i], TOTAL_TABLE_SIZE);
    const int delta = i - pos;
    hue_linearity_search_range(0) =
        fminf(hue_linearity_search_range(0), static_cast<float>(delta + lower_padding));
    hue_linearity_search_range(1) =
        fmaxf(hue_linearity_search_range(1), static_cast<float>(delta + upper_padding));
  }

  return hue_linearity_search_range;
}
}  // namespace ColorUtils
};  // namespace alcedo
