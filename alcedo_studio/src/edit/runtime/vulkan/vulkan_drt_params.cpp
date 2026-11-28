//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_drt_params.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace alcedo {
namespace {

static_assert(ALCEDO_VK_DRT_TABLE_SIZE == TOTAL_TABLE_SIZE);
static_assert(ALCEDO_VK_DRT_ACES_END - ALCEDO_VK_DRT_ACES == 2680,
              "OpenClODTParams has 10 720 bytes");
static_assert(ALCEDO_VK_DRT_ODRT_END - ALCEDO_VK_DRT_ODRT == 82,
              "OpenClOpenDRTParams has 328 bytes");

void PutFloat(VulkanDrtParamBlock& block, int word, float value) {
  block[static_cast<std::size_t>(word)] = std::bit_cast<std::uint32_t>(value);
}

void PutInt(VulkanDrtParamBlock& block, int word, std::int32_t value) {
  block[static_cast<std::size_t>(word)] = static_cast<std::uint32_t>(value);
}

/// The nine values of @p matrix in the order of `Copy33` of the OpenCL packing (row by row).
void PutMatrix(VulkanDrtParamBlock& block, int word, const cv::Matx33f& matrix) {
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      PutFloat(block, word + row * 3 + column, matrix(row, column));
    }
  }
}

void PutJmh(VulkanDrtParamBlock& block, int word, const ColorUtils::JMhParams& source) {
  PutMatrix(block, word + ALCEDO_VK_DRT_JMH_RGB_TO_CAM16, source.MATRIX_RGB_to_CAM16_c_);
  PutMatrix(block, word + ALCEDO_VK_DRT_JMH_CAM16_TO_RGB, source.MATRIX_CAM16_c_to_RGB_);
  PutMatrix(block, word + ALCEDO_VK_DRT_JMH_CONE_TO_AAB, source.MATRIX_cone_response_to_Aab_);
  PutMatrix(block, word + ALCEDO_VK_DRT_JMH_AAB_TO_CONE, source.MATRIX_Aab_to_cone_response_);
  PutFloat(block, word + ALCEDO_VK_DRT_JMH_F_L_N, source.F_L_n_);
  PutFloat(block, word + ALCEDO_VK_DRT_JMH_CZ, source.cz_);
  PutFloat(block, word + ALCEDO_VK_DRT_JMH_INV_CZ, source.inv_cz_);
  PutFloat(block, word + ALCEDO_VK_DRT_JMH_A_W_J,
           source.inv_A_w_J_ != 0.0F ? 1.0F / source.inv_A_w_J_ : 0.0F);
  PutFloat(block, word + ALCEDO_VK_DRT_JMH_INV_A_W_J, source.inv_A_w_J_);
}

void PutAces(VulkanDrtParamBlock& block, const ColorUtils::ODTParams& source, bool tables) {
  PutFloat(block, ALCEDO_VK_DRT_ACES_PEAK_LUMINANCE, source.peak_luminance_);
  PutJmh(block, ALCEDO_VK_DRT_ACES_INPUT, source.input_params_);
  PutJmh(block, ALCEDO_VK_DRT_ACES_REACH, source.reach_params_);
  PutJmh(block, ALCEDO_VK_DRT_ACES_LIMIT, source.limit_params_);
  const auto& ts = source.ts_params_;
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_N, ts.n_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_N_R, ts.n_r_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_G, ts.g_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_T_1, ts.t_1_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_C_T, ts.c_t_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_S_2, ts.s_2_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_U_2, ts.u_2_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_M_2, ts.m_2_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_FORWARD_LIMIT, ts.forward_limit_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_INVERSE_LIMIT, ts.inverse_limit_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_TS + ALCEDO_VK_DRT_TS_LOG_PEAK, ts.log_peak_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_LIMIT_J_MAX, source.limit_J_max_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_MODEL_GAMMA_INV, source.model_gamma_inv_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_MID_J, source.mid_J_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_FOCUS_DIST, source.focus_dist_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_LOWER_HULL_GAMMA_INV, source.lower_hull_gamma_inv_);
  PutInt(block, ALCEDO_VK_DRT_ACES_HUE_SEARCH_LO,
         static_cast<std::int32_t>(source.hue_linearity_search_range_(0)));
  PutInt(block, ALCEDO_VK_DRT_ACES_HUE_SEARCH_HI,
         static_cast<std::int32_t>(source.hue_linearity_search_range_(1)));
  PutFloat(block, ALCEDO_VK_DRT_ACES_SAT, source.sat_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_SAT_THR, source.sat_thr_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_COMPR, source.compr_);
  PutFloat(block, ALCEDO_VK_DRT_ACES_CHROMA_COMPRESS_SCALE, source.chroma_compress_scale_);
  if (!tables) {
    return;
  }
  if (!source.table_reach_M_ || !source.table_hues_ || !source.table_upper_hull_gammas_ ||
      !source.table_gamut_cusps_) {
    throw std::runtime_error("PackVulkanDrtParams: ACES 2.0 tables were not resolved");
  }
  for (int index = 0; index < ALCEDO_VK_DRT_TABLE_SIZE; ++index) {
    const auto at = static_cast<std::size_t>(index);
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_REACH_M + index, (*source.table_reach_M_)[at]);
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_HUES + index, (*source.table_hues_)[at]);
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_UPPER_HULL_GAMMA + index,
             (*source.table_upper_hull_gammas_)[at]);
    const auto& cusp = (*source.table_gamut_cusps_)[at];
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * index + 0, cusp(0));
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * index + 1, cusp(1));
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * index + 2, cusp(2));
    PutFloat(block, ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * index + 3, 0.0F);
  }
}

void PutOpenDrt(VulkanDrtParamBlock& block, const ColorUtils::OpenDRTParams& s) {
  PutInt(block, ALCEDO_VK_DRT_ODRT_TN_HCON_ENABLE, s.tn_hcon_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_TN_LCON_ENABLE, s.tn_lcon_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_PT_ENABLE, s.pt_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_PTL_ENABLE, s.ptl_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_PTM_ENABLE, s.ptm_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_BRL_ENABLE, s.brl_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_BRLP_ENABLE, s.brlp_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_HC_ENABLE, s.hc_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_HS_RGB_ENABLE, s.hs_rgb_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_HS_CMY_ENABLE, s.hs_cmy_enable_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_CREATIVE_WHITE, s.creative_white_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_SURROUND, s.surround_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_CLAMP, s.clamp_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_DISPLAY_GAMUT, s.display_gamut_);
  PutInt(block, ALCEDO_VK_DRT_ODRT_DISPLAY_EOTF, s.display_eotf_);
  const float values[] = {
      s.tn_con_,      s.tn_sh_,      s.tn_toe_,    s.tn_off_,       s.tn_hcon_,     s.tn_hcon_pv_,
      s.tn_hcon_st_,  s.tn_lcon_,    s.tn_lcon_w_, s.cwp_lm_,       s.rs_sa_,       s.rs_rw_,
      s.rs_bw_,       s.pt_lml_,     s.pt_lml_r_,  s.pt_lml_g_,     s.pt_lml_b_,    s.pt_lmh_,
      s.pt_lmh_r_,    s.pt_lmh_b_,   s.ptl_c_,     s.ptl_m_,        s.ptl_y_,       s.ptm_low_,
      s.ptm_low_rng_, s.ptm_low_st_, s.ptm_high_,  s.ptm_high_rng_, s.ptm_high_st_, s.brl_,
      s.brl_r_,       s.brl_g_,      s.brl_b_,     s.brl_rng_,      s.brl_st_,      s.brlp_,
      s.brlp_r_,      s.brlp_g_,     s.brlp_b_,    s.hc_r_,         s.hc_r_rng_,    s.hs_r_,
      s.hs_r_rng_,    s.hs_g_,       s.hs_g_rng_,  s.hs_b_,         s.hs_b_rng_,    s.hs_c_,
      s.hs_c_rng_,    s.hs_m_,       s.hs_m_rng_,  s.hs_y_,         s.hs_y_rng_,    s.ts_x1_,
      s.ts_y1_,       s.ts_x0_,      s.ts_y0_,     s.ts_s0_,        s.ts_p_,        s.ts_s10_,
      s.ts_m1_,       s.ts_m2_,      s.ts_s_,      s.ts_dsc_,       s.pt_cmp_Lf_,   s.s_Lp100_,
      s.ts_s1_};
  static_assert(sizeof(values) / sizeof(float) ==
                ALCEDO_VK_DRT_ODRT_END - ALCEDO_VK_DRT_ODRT_TN_CON);
  for (int index = 0; index < ALCEDO_VK_DRT_ODRT_END - ALCEDO_VK_DRT_ODRT_TN_CON; ++index) {
    PutFloat(block, ALCEDO_VK_DRT_ODRT_TN_CON + index, values[index]);
  }
}

}  // namespace

auto PackVulkanDrtParams(const ColorUtils::TO_OUTPUT_Params& resolved) -> VulkanDrtParamBlock {
  VulkanDrtParamBlock block{};
  PutInt(block, ALCEDO_VK_DRT_METHOD, static_cast<std::int32_t>(resolved.method_));
  PutInt(block, ALCEDO_VK_DRT_EOTF, static_cast<std::int32_t>(resolved.eotf_));
  PutAces(block, resolved.aces_params_, resolved.method_ == ColorUtils::ODTMethod::ACES_2_0);
  PutOpenDrt(block, resolved.open_drt_params_);
  PutMatrix(block, ALCEDO_VK_DRT_LIMIT_TO_DISPLAY, resolved.limit_to_display_matx_);
  PutFloat(block, ALCEDO_VK_DRT_DISPLAY_LINEAR_SCALE, resolved.display_linear_scale_);
  return block;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
