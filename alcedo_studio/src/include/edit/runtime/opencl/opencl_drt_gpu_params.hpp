//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "edit/operators/utils/color_utils.hpp"

namespace alcedo::OpenCL::Pipeline {

// DRT parameter structs that the OpenCL DRT program reads from the parameter arena. The layout
// matches `OpenClToOutputParams` in `edit/runtime/opencl/shader/drt_params.cl`.
inline constexpr int32_t kOpenClAcesOdtTableSize = TOTAL_TABLE_SIZE;

struct OpenClJMhParams {
  float MATRIX_RGB_to_CAM16_c_[9]       = {};
  float MATRIX_CAM16_c_to_RGB_[9]       = {};
  float MATRIX_cone_response_to_Aab_[9] = {};
  float MATRIX_Aab_to_cone_response_[9] = {};
  float F_L_n_                          = 0.0f;
  float cz_                             = 0.0f;
  float inv_cz_                         = 0.0f;
  float A_w_J_                          = 0.0f;
  float inv_A_w_J_                      = 0.0f;
};

struct OpenClTSParams {
  float n_             = 0.0f;
  float n_r_           = 0.0f;
  float g_             = 0.0f;
  float t_1_           = 0.0f;
  float c_t_           = 0.0f;
  float s_2_           = 0.0f;
  float u_2_           = 0.0f;
  float m_2_           = 0.0f;
  float forward_limit_ = 0.0f;
  float inverse_limit_ = 0.0f;
  float log_peak_      = 0.0f;
};

struct OpenClODTParams {
  float           peak_luminance_                                  = 100.0f;
  OpenClJMhParams input_params_                                    = {};
  OpenClJMhParams reach_params_                                    = {};
  OpenClJMhParams limit_params_                                    = {};
  OpenClTSParams  ts_                                              = {};
  float           limit_J_max                                      = 0.0f;
  float           model_gamma_inv                                  = 0.0f;
  float           mid_J                                            = 0.0f;
  float           focus_dist                                       = 0.0f;
  float           lower_hull_gamma_inv                             = 0.0f;
  int32_t         hue_linearity_search_range[2]                    = {0, 1};
  float           sat                                              = 0.0f;
  float           sat_thr                                          = 0.0f;
  float           compr                                            = 0.0f;
  float           chroma_compress_scale                            = 0.0f;
  float           table_reach_M_[kOpenClAcesOdtTableSize]          = {};
  float           table_hues_[kOpenClAcesOdtTableSize]             = {};
  float           table_upper_hull_gamma_[kOpenClAcesOdtTableSize] = {};
  float           table_gamut_cusps_[kOpenClAcesOdtTableSize][4]   = {};
};

struct OpenClOpenDRTParams {
  int32_t tn_hcon_enable_ = 0;
  int32_t tn_lcon_enable_ = 0;
  int32_t pt_enable_      = 1;
  int32_t ptl_enable_     = 1;
  int32_t ptm_enable_     = 1;
  int32_t brl_enable_     = 1;
  int32_t brlp_enable_    = 1;
  int32_t hc_enable_      = 1;
  int32_t hs_rgb_enable_  = 1;
  int32_t hs_cmy_enable_  = 1;
  int32_t creative_white_ = 2;
  int32_t surround_       = 2;
  int32_t clamp_          = 1;
  int32_t display_gamut_  = 0;
  int32_t display_eotf_   = 1;
  float   tn_con_         = 1.66f;
  float   tn_sh_          = 0.5f;
  float   tn_toe_         = 0.003f;
  float   tn_off_         = 0.005f;
  float   tn_hcon_        = 0.0f;
  float   tn_hcon_pv_     = 1.0f;
  float   tn_hcon_st_     = 4.0f;
  float   tn_lcon_        = 0.0f;
  float   tn_lcon_w_      = 0.5f;
  float   cwp_lm_         = 0.25f;
  float   rs_sa_          = 0.35f;
  float   rs_rw_          = 0.25f;
  float   rs_bw_          = 0.55f;
  float   pt_lml_         = 0.25f;
  float   pt_lml_r_       = 0.5f;
  float   pt_lml_g_       = 0.0f;
  float   pt_lml_b_       = 0.1f;
  float   pt_lmh_         = 0.25f;
  float   pt_lmh_r_       = 0.5f;
  float   pt_lmh_b_       = 0.0f;
  float   ptl_c_          = 0.06f;
  float   ptl_m_          = 0.08f;
  float   ptl_y_          = 0.06f;
  float   ptm_low_        = 0.4f;
  float   ptm_low_rng_    = 0.25f;
  float   ptm_low_st_     = 0.5f;
  float   ptm_high_       = -0.8f;
  float   ptm_high_rng_   = 0.35f;
  float   ptm_high_st_    = 0.4f;
  float   brl_            = 0.0f;
  float   brl_r_          = -2.5f;
  float   brl_g_          = -1.5f;
  float   brl_b_          = -1.5f;
  float   brl_rng_        = 0.5f;
  float   brl_st_         = 0.35f;
  float   brlp_           = -0.5f;
  float   brlp_r_         = -1.25f;
  float   brlp_g_         = -1.25f;
  float   brlp_b_         = -0.25f;
  float   hc_r_           = 1.0f;
  float   hc_r_rng_       = 0.3f;
  float   hs_r_           = 0.6f;
  float   hs_r_rng_       = 0.6f;
  float   hs_g_           = 0.35f;
  float   hs_g_rng_       = 1.0f;
  float   hs_b_           = 0.66f;
  float   hs_b_rng_       = 1.0f;
  float   hs_c_           = 0.25f;
  float   hs_c_rng_       = 1.0f;
  float   hs_m_           = 0.0f;
  float   hs_m_rng_       = 1.0f;
  float   hs_y_           = 0.0f;
  float   hs_y_rng_       = 1.0f;
  float   ts_x1_          = 0.0f;
  float   ts_y1_          = 0.0f;
  float   ts_x0_          = 0.0f;
  float   ts_y0_          = 0.0f;
  float   ts_s0_          = 0.0f;
  float   ts_p_           = 0.0f;
  float   ts_s10_         = 0.0f;
  float   ts_m1_          = 0.0f;
  float   ts_m2_          = 0.0f;
  float   ts_s_           = 0.0f;
  float   ts_dsc_         = 0.0f;
  float   pt_cmp_Lf_      = 0.0f;
  float   s_Lp100_        = 0.0f;
  float   ts_s1_          = 0.0f;
};

struct OpenClToOutputParams {
  int32_t             method_          = static_cast<int32_t>(ColorUtils::ODTMethod::OPEN_DRT);
  int32_t             eotf_            = static_cast<int32_t>(ColorUtils::EOTF::LINEAR);
  OpenClODTParams     aces_params_     = {};
  OpenClOpenDRTParams open_drt_params_ = {};
  float               limit_to_display_matx[9] = {};
  float               display_linear_scale_    = 1.0f;
};

// Pre-G10.5 `OpenClToOutputParams` was 11096 bytes with no padding (commit 0cf45f45). The OpenCL
// program reads the same fields in the same order, so every member must follow the previous one.
static_assert(std::is_standard_layout_v<OpenClToOutputParams>);
static_assert(sizeof(OpenClToOutputParams) == 11096);
static_assert(offsetof(OpenClToOutputParams, eotf_) == 4);
static_assert(offsetof(OpenClToOutputParams, aces_params_) == 8);
static_assert(offsetof(OpenClToOutputParams, open_drt_params_) == 8 + sizeof(OpenClODTParams));
static_assert(offsetof(OpenClToOutputParams, limit_to_display_matx) ==
              offsetof(OpenClToOutputParams, open_drt_params_) + sizeof(OpenClOpenDRTParams));
static_assert(offsetof(OpenClToOutputParams, display_linear_scale_) ==
              offsetof(OpenClToOutputParams, limit_to_display_matx) + 9 * sizeof(float));

}  // namespace alcedo::OpenCL::Pipeline

#endif  // HAVE_OPENCL
