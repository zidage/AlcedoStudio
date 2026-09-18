//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "json.hpp"

namespace alcedo {

inline constexpr int kMetalDrtTableSize = 362;

struct MetalDrtJmhParams {
  float matrix_rgb_to_cam16[9]       = {};
  float matrix_cam16_to_rgb[9]       = {};
  float matrix_cone_to_aab[9]        = {};
  float matrix_aab_to_cone[9]        = {};
  float f_l_n                        = 0.0f;
  float cz                           = 0.0f;
  float inv_cz                       = 0.0f;
  float a_w_j                        = 0.0f;
  float inv_a_w_j                    = 0.0f;
};

struct MetalDrtTsParams {
  float n             = 0.0f;
  float n_r           = 0.0f;
  float g             = 0.0f;
  float t_1           = 0.0f;
  float c_t           = 0.0f;
  float s_2           = 0.0f;
  float u_2           = 0.0f;
  float m_2           = 0.0f;
  float forward_limit = 0.0f;
  float inverse_limit = 0.0f;
  float log_peak      = 0.0f;
};

struct MetalDrtAcesParams {
  float            peak_luminance = 100.0f;
  MetalDrtJmhParams input{};
  MetalDrtJmhParams reach{};
  MetalDrtJmhParams limit{};
  MetalDrtTsParams  ts{};
  float            limit_j_max          = 0.0f;
  float            model_gamma_inv      = 0.0f;
  float            mid_j                = 0.0f;
  float            focus_dist           = 0.0f;
  float            lower_hull_gamma_inv = 0.0f;
  std::int32_t     hue_linearity_search_range[2] = {0, 1};
  float            sat                   = 0.0f;
  float            sat_thr               = 0.0f;
  float            compr                 = 0.0f;
  float            chroma_compress_scale = 0.0f;
  float            table_reach_m[kMetalDrtTableSize]          = {};
  float            table_hues[kMetalDrtTableSize]             = {};
  float            table_upper_hull_gamma[kMetalDrtTableSize] = {};
  float            table_gamut_cusps[kMetalDrtTableSize][4]   = {};
};

struct MetalDrtOpenParams {
  std::int32_t tn_hcon_enable = 0;
  std::int32_t tn_lcon_enable = 0;
  std::int32_t pt_enable      = 1;
  std::int32_t ptl_enable     = 1;
  std::int32_t ptm_enable     = 1;
  std::int32_t brl_enable     = 1;
  std::int32_t brlp_enable    = 1;
  std::int32_t hc_enable      = 1;
  std::int32_t hs_rgb_enable  = 1;
  std::int32_t hs_cmy_enable  = 1;
  std::int32_t creative_white = 2;
  std::int32_t surround       = 2;
  std::int32_t clamp          = 1;
  std::int32_t display_gamut  = 0;
  std::int32_t display_eotf   = 1;
  float        tn_con         = 1.66f;
  float        tn_sh          = 0.5f;
  float        tn_toe         = 0.003f;
  float        tn_off         = 0.005f;
  float        tn_hcon        = 0.0f;
  float        tn_hcon_pv     = 1.0f;
  float        tn_hcon_st     = 4.0f;
  float        tn_lcon        = 0.0f;
  float        tn_lcon_w      = 0.5f;
  float        cwp_lm         = 0.25f;
  float        rs_sa          = 0.35f;
  float        rs_rw          = 0.25f;
  float        rs_bw          = 0.55f;
  float        pt_lml         = 0.25f;
  float        pt_lml_r       = 0.5f;
  float        pt_lml_g       = 0.0f;
  float        pt_lml_b       = 0.1f;
  float        pt_lmh         = 0.25f;
  float        pt_lmh_r       = 0.5f;
  float        pt_lmh_b       = 0.0f;
  float        ptl_c          = 0.06f;
  float        ptl_m          = 0.08f;
  float        ptl_y          = 0.06f;
  float        ptm_low        = 0.4f;
  float        ptm_low_rng    = 0.25f;
  float        ptm_low_st     = 0.5f;
  float        ptm_high       = -0.8f;
  float        ptm_high_rng   = 0.35f;
  float        ptm_high_st    = 0.4f;
  float        brl            = 0.0f;
  float        brl_r          = -2.5f;
  float        brl_g          = -1.5f;
  float        brl_b          = -1.5f;
  float        brl_rng        = 0.5f;
  float        brl_st         = 0.35f;
  float        brlp           = -0.5f;
  float        brlp_r         = -1.25f;
  float        brlp_g         = -1.25f;
  float        brlp_b         = -0.25f;
  float        hc_r           = 1.0f;
  float        hc_r_rng       = 0.3f;
  float        hs_r           = 0.6f;
  float        hs_r_rng       = 0.6f;
  float        hs_g           = 0.35f;
  float        hs_g_rng       = 1.0f;
  float        hs_b           = 0.66f;
  float        hs_b_rng       = 1.0f;
  float        hs_c           = 0.25f;
  float        hs_c_rng       = 1.0f;
  float        hs_m           = 0.0f;
  float        hs_m_rng       = 1.0f;
  float        hs_y           = 0.0f;
  float        hs_y_rng       = 1.0f;
  float        ts_x1          = 0.0f;
  float        ts_y1          = 0.0f;
  float        ts_x0          = 0.0f;
  float        ts_y0          = 0.0f;
  float        ts_s0          = 0.0f;
  float        ts_p           = 0.0f;
  float        ts_s10         = 0.0f;
  float        ts_m1          = 0.0f;
  float        ts_m2          = 0.0f;
  float        ts_s           = 0.0f;
  float        ts_dsc         = 0.0f;
  float        pt_cmp_lf      = 0.0f;
  float        s_lp100        = 0.0f;
  float        ts_s1          = 0.0f;
};

struct MetalDrtGpuParams {
  std::int32_t       method                  = 1;
  std::int32_t       eotf                    = 0;
  MetalDrtAcesParams aces{};
  MetalDrtOpenParams open_drt{};
  float              limit_to_display_matx[9] = {};
  float              display_linear_scale     = 1.0f;
};

[[nodiscard]] auto ResolveMetalDrtGpuParams(const nlohmann::json& odt_json) -> MetalDrtGpuParams;

}  // namespace alcedo
