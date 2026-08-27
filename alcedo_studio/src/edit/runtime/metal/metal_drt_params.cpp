//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_drt_gpu_params.hpp"

#include <cstring>
#include <stdexcept>

#include "edit/operators/cst/odt_op.hpp"

namespace alcedo {
namespace {

void Copy33(const cv::Matx33f& matrix, float out[9]) {
  out[0] = matrix(0, 0);
  out[1] = matrix(0, 1);
  out[2] = matrix(0, 2);
  out[3] = matrix(1, 0);
  out[4] = matrix(1, 1);
  out[5] = matrix(1, 2);
  out[6] = matrix(2, 0);
  out[7] = matrix(2, 1);
  out[8] = matrix(2, 2);
}

void CopyJmh(const ColorUtils::JMhParams& src, MetalDrtJmhParams& dst) {
  Copy33(src.MATRIX_RGB_to_CAM16_c_, dst.matrix_rgb_to_cam16);
  Copy33(src.MATRIX_CAM16_c_to_RGB_, dst.matrix_cam16_to_rgb);
  Copy33(src.MATRIX_cone_response_to_Aab_, dst.matrix_cone_to_aab);
  Copy33(src.MATRIX_Aab_to_cone_response_, dst.matrix_aab_to_cone);
  dst.f_l_n     = src.F_L_n_;
  dst.cz        = src.cz_;
  dst.inv_cz    = src.inv_cz_;
  dst.a_w_j     = (src.inv_A_w_J_ != 0.0f) ? (1.0f / src.inv_A_w_J_) : 0.0f;
  dst.inv_a_w_j = src.inv_A_w_J_;
}

}  // namespace

auto ResolveMetalDrtGpuParams(const nlohmann::json& odt_json) -> MetalDrtGpuParams {
  ODT_Op         descriptor(nlohmann::json{{"odt", odt_json}});
  OperatorParams cpu_params;
  descriptor.SetGlobalParams(cpu_params);
  const auto&                cpu = cpu_params.to_output_params_;
  MetalDrtGpuParams          gpu;
  gpu.method               = static_cast<std::int32_t>(cpu.method_);
  gpu.eotf                 = static_cast<std::int32_t>(cpu.eotf_);
  Copy33(cpu.limit_to_display_matx_, gpu.limit_to_display_matx);
  gpu.display_linear_scale = cpu.display_linear_scale_;

  const auto& odt_cpu            = cpu.aces_params_;
  auto&       odt                = gpu.aces;
  odt.peak_luminance             = odt_cpu.peak_luminance_;
  CopyJmh(odt_cpu.input_params_, odt.input);
  CopyJmh(odt_cpu.reach_params_, odt.reach);
  CopyJmh(odt_cpu.limit_params_, odt.limit);
  odt.ts.n                       = odt_cpu.ts_params_.n_;
  odt.ts.n_r                     = odt_cpu.ts_params_.n_r_;
  odt.ts.g                       = odt_cpu.ts_params_.g_;
  odt.ts.t_1                     = odt_cpu.ts_params_.t_1_;
  odt.ts.c_t                     = odt_cpu.ts_params_.c_t_;
  odt.ts.s_2                     = odt_cpu.ts_params_.s_2_;
  odt.ts.u_2                     = odt_cpu.ts_params_.u_2_;
  odt.ts.m_2                     = odt_cpu.ts_params_.m_2_;
  odt.ts.forward_limit           = odt_cpu.ts_params_.forward_limit_;
  odt.ts.inverse_limit           = odt_cpu.ts_params_.inverse_limit_;
  odt.ts.log_peak                = odt_cpu.ts_params_.log_peak_;
  odt.limit_j_max                = odt_cpu.limit_J_max_;
  odt.model_gamma_inv            = odt_cpu.model_gamma_inv_;
  odt.mid_j                      = odt_cpu.mid_J_;
  odt.focus_dist                 = odt_cpu.focus_dist_;
  odt.lower_hull_gamma_inv       = odt_cpu.lower_hull_gamma_inv_;
  odt.hue_linearity_search_range[0] = static_cast<std::int32_t>(odt_cpu.hue_linearity_search_range_(0));
  odt.hue_linearity_search_range[1] = static_cast<std::int32_t>(odt_cpu.hue_linearity_search_range_(1));
  odt.sat                       = odt_cpu.sat_;
  odt.sat_thr                   = odt_cpu.sat_thr_;
  odt.compr                     = odt_cpu.compr_;
  odt.chroma_compress_scale     = odt_cpu.chroma_compress_scale_;
  if (cpu.method_ == ColorUtils::ODTMethod::ACES_2_0) {
    if (!odt_cpu.table_reach_M_ || !odt_cpu.table_hues_ || !odt_cpu.table_upper_hull_gammas_ ||
        !odt_cpu.table_gamut_cusps_) {
      throw std::runtime_error("ExecuteMetalDrt: ACES 2.0 tables were not resolved");
    }
    std::memcpy(odt.table_reach_m, odt_cpu.table_reach_M_->data(), sizeof(odt.table_reach_m));
    std::memcpy(odt.table_hues, odt_cpu.table_hues_->data(), sizeof(odt.table_hues));
    std::memcpy(odt.table_upper_hull_gamma, odt_cpu.table_upper_hull_gammas_->data(),
                sizeof(odt.table_upper_hull_gamma));
    for (int i = 0; i < kMetalDrtTableSize; ++i) {
      const auto& cusp            = (*odt_cpu.table_gamut_cusps_)[static_cast<std::size_t>(i)];
      odt.table_gamut_cusps[i][0] = cusp(0);
      odt.table_gamut_cusps[i][1] = cusp(1);
      odt.table_gamut_cusps[i][2] = cusp(2);
      odt.table_gamut_cusps[i][3] = 0.0f;
    }
  }

  const auto& open_cpu     = cpu.open_drt_params_;
  auto&       open         = gpu.open_drt;
  open.tn_hcon_enable      = open_cpu.tn_hcon_enable_;
  open.tn_lcon_enable      = open_cpu.tn_lcon_enable_;
  open.pt_enable           = open_cpu.pt_enable_;
  open.ptl_enable          = open_cpu.ptl_enable_;
  open.ptm_enable          = open_cpu.ptm_enable_;
  open.brl_enable          = open_cpu.brl_enable_;
  open.brlp_enable         = open_cpu.brlp_enable_;
  open.hc_enable           = open_cpu.hc_enable_;
  open.hs_rgb_enable       = open_cpu.hs_rgb_enable_;
  open.hs_cmy_enable       = open_cpu.hs_cmy_enable_;
  open.creative_white      = open_cpu.creative_white_;
  open.surround            = open_cpu.surround_;
  open.clamp               = open_cpu.clamp_;
  open.display_gamut       = open_cpu.display_gamut_;
  open.display_eotf        = open_cpu.display_eotf_;
  open.tn_con              = open_cpu.tn_con_;
  open.tn_sh               = open_cpu.tn_sh_;
  open.tn_toe              = open_cpu.tn_toe_;
  open.tn_off              = open_cpu.tn_off_;
  open.tn_hcon             = open_cpu.tn_hcon_;
  open.tn_hcon_pv          = open_cpu.tn_hcon_pv_;
  open.tn_hcon_st          = open_cpu.tn_hcon_st_;
  open.tn_lcon             = open_cpu.tn_lcon_;
  open.tn_lcon_w           = open_cpu.tn_lcon_w_;
  open.cwp_lm              = open_cpu.cwp_lm_;
  open.rs_sa               = open_cpu.rs_sa_;
  open.rs_rw               = open_cpu.rs_rw_;
  open.rs_bw               = open_cpu.rs_bw_;
  open.pt_lml              = open_cpu.pt_lml_;
  open.pt_lml_r            = open_cpu.pt_lml_r_;
  open.pt_lml_g            = open_cpu.pt_lml_g_;
  open.pt_lml_b            = open_cpu.pt_lml_b_;
  open.pt_lmh              = open_cpu.pt_lmh_;
  open.pt_lmh_r            = open_cpu.pt_lmh_r_;
  open.pt_lmh_b            = open_cpu.pt_lmh_b_;
  open.ptl_c               = open_cpu.ptl_c_;
  open.ptl_m               = open_cpu.ptl_m_;
  open.ptl_y               = open_cpu.ptl_y_;
  open.ptm_low             = open_cpu.ptm_low_;
  open.ptm_low_rng         = open_cpu.ptm_low_rng_;
  open.ptm_low_st          = open_cpu.ptm_low_st_;
  open.ptm_high            = open_cpu.ptm_high_;
  open.ptm_high_rng        = open_cpu.ptm_high_rng_;
  open.ptm_high_st         = open_cpu.ptm_high_st_;
  open.brl                 = open_cpu.brl_;
  open.brl_r               = open_cpu.brl_r_;
  open.brl_g               = open_cpu.brl_g_;
  open.brl_b               = open_cpu.brl_b_;
  open.brl_rng             = open_cpu.brl_rng_;
  open.brl_st              = open_cpu.brl_st_;
  open.brlp                = open_cpu.brlp_;
  open.brlp_r              = open_cpu.brlp_r_;
  open.brlp_g              = open_cpu.brlp_g_;
  open.brlp_b              = open_cpu.brlp_b_;
  open.hc_r                = open_cpu.hc_r_;
  open.hc_r_rng            = open_cpu.hc_r_rng_;
  open.hs_r                = open_cpu.hs_r_;
  open.hs_r_rng            = open_cpu.hs_r_rng_;
  open.hs_g                = open_cpu.hs_g_;
  open.hs_g_rng            = open_cpu.hs_g_rng_;
  open.hs_b                = open_cpu.hs_b_;
  open.hs_b_rng            = open_cpu.hs_b_rng_;
  open.hs_c                = open_cpu.hs_c_;
  open.hs_c_rng            = open_cpu.hs_c_rng_;
  open.hs_m                = open_cpu.hs_m_;
  open.hs_m_rng            = open_cpu.hs_m_rng_;
  open.hs_y                = open_cpu.hs_y_;
  open.hs_y_rng            = open_cpu.hs_y_rng_;
  open.ts_x1               = open_cpu.ts_x1_;
  open.ts_y1               = open_cpu.ts_y1_;
  open.ts_x0               = open_cpu.ts_x0_;
  open.ts_y0               = open_cpu.ts_y0_;
  open.ts_s0               = open_cpu.ts_s0_;
  open.ts_p                 = open_cpu.ts_p_;
  open.ts_s10               = open_cpu.ts_s10_;
  open.ts_m1               = open_cpu.ts_m1_;
  open.ts_m2               = open_cpu.ts_m2_;
  open.ts_s                 = open_cpu.ts_s_;
  open.ts_dsc               = open_cpu.ts_dsc_;
  open.pt_cmp_lf           = open_cpu.pt_cmp_Lf_;
  open.s_lp100             = open_cpu.s_Lp100_;
  open.ts_s1               = open_cpu.ts_s1_;
  return gpu;
}

}  // namespace alcedo
