//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_drt_params.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "edit/operators/cst/odt_op.hpp"

namespace alcedo {
namespace {

using OpenClODTParams     = OpenCL::Pipeline::OpenClODTParams;
using OpenClJMhParams     = OpenCL::Pipeline::OpenClJMhParams;
using OpenClOpenDRTParams = OpenCL::Pipeline::OpenClOpenDRTParams;

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

void CopyJmh(const ColorUtils::JMhParams& source, OpenClJMhParams& destination) {
  Copy33(source.MATRIX_RGB_to_CAM16_c_, destination.MATRIX_RGB_to_CAM16_c_);
  Copy33(source.MATRIX_CAM16_c_to_RGB_, destination.MATRIX_CAM16_c_to_RGB_);
  Copy33(source.MATRIX_cone_response_to_Aab_, destination.MATRIX_cone_response_to_Aab_);
  Copy33(source.MATRIX_Aab_to_cone_response_, destination.MATRIX_Aab_to_cone_response_);
  destination.F_L_n_     = source.F_L_n_;
  destination.cz_        = source.cz_;
  destination.inv_cz_    = source.inv_cz_;
  destination.A_w_J_     = source.inv_A_w_J_ != 0.0f ? 1.0f / source.inv_A_w_J_ : 0.0f;
  destination.inv_A_w_J_ = source.inv_A_w_J_;
}

void CopyAces(const ColorUtils::ODTParams& source, OpenClODTParams& destination) {
  destination.peak_luminance_      = source.peak_luminance_;
  destination.limit_J_max          = source.limit_J_max_;
  destination.model_gamma_inv      = source.model_gamma_inv_;
  destination.mid_J                = source.mid_J_;
  destination.focus_dist           = source.focus_dist_;
  destination.lower_hull_gamma_inv = source.lower_hull_gamma_inv_;
  destination.hue_linearity_search_range[0] =
      static_cast<std::int32_t>(source.hue_linearity_search_range_(0));
  destination.hue_linearity_search_range[1] =
      static_cast<std::int32_t>(source.hue_linearity_search_range_(1));
  destination.sat                   = source.sat_;
  destination.sat_thr               = source.sat_thr_;
  destination.compr                 = source.compr_;
  destination.chroma_compress_scale = source.chroma_compress_scale_;
  CopyJmh(source.input_params_, destination.input_params_);
  CopyJmh(source.reach_params_, destination.reach_params_);
  CopyJmh(source.limit_params_, destination.limit_params_);

  destination.ts_.n_             = source.ts_params_.n_;
  destination.ts_.n_r_           = source.ts_params_.n_r_;
  destination.ts_.g_             = source.ts_params_.g_;
  destination.ts_.t_1_           = source.ts_params_.t_1_;
  destination.ts_.c_t_           = source.ts_params_.c_t_;
  destination.ts_.s_2_           = source.ts_params_.s_2_;
  destination.ts_.u_2_           = source.ts_params_.u_2_;
  destination.ts_.m_2_           = source.ts_params_.m_2_;
  destination.ts_.forward_limit_ = source.ts_params_.forward_limit_;
  destination.ts_.inverse_limit_ = source.ts_params_.inverse_limit_;
  destination.ts_.log_peak_      = source.ts_params_.log_peak_;
}

void CopyOpenDrt(const ColorUtils::OpenDRTParams& source, OpenClOpenDRTParams& destination) {
  destination.tn_hcon_enable_ = source.tn_hcon_enable_;
  destination.tn_lcon_enable_ = source.tn_lcon_enable_;
  destination.pt_enable_      = source.pt_enable_;
  destination.ptl_enable_     = source.ptl_enable_;
  destination.ptm_enable_     = source.ptm_enable_;
  destination.brl_enable_     = source.brl_enable_;
  destination.brlp_enable_    = source.brlp_enable_;
  destination.hc_enable_      = source.hc_enable_;
  destination.hs_rgb_enable_  = source.hs_rgb_enable_;
  destination.hs_cmy_enable_  = source.hs_cmy_enable_;
  destination.creative_white_ = source.creative_white_;
  destination.surround_       = source.surround_;
  destination.clamp_          = source.clamp_;
  destination.display_gamut_  = source.display_gamut_;
  destination.display_eotf_   = source.display_eotf_;
  destination.tn_con_         = source.tn_con_;
  destination.tn_sh_          = source.tn_sh_;
  destination.tn_toe_         = source.tn_toe_;
  destination.tn_off_         = source.tn_off_;
  destination.tn_hcon_        = source.tn_hcon_;
  destination.tn_hcon_pv_     = source.tn_hcon_pv_;
  destination.tn_hcon_st_     = source.tn_hcon_st_;
  destination.tn_lcon_        = source.tn_lcon_;
  destination.tn_lcon_w_      = source.tn_lcon_w_;
  destination.cwp_lm_         = source.cwp_lm_;
  destination.rs_sa_          = source.rs_sa_;
  destination.rs_rw_          = source.rs_rw_;
  destination.rs_bw_          = source.rs_bw_;
  destination.pt_lml_         = source.pt_lml_;
  destination.pt_lml_r_       = source.pt_lml_r_;
  destination.pt_lml_g_       = source.pt_lml_g_;
  destination.pt_lml_b_       = source.pt_lml_b_;
  destination.pt_lmh_         = source.pt_lmh_;
  destination.pt_lmh_r_       = source.pt_lmh_r_;
  destination.pt_lmh_b_       = source.pt_lmh_b_;
  destination.ptl_c_          = source.ptl_c_;
  destination.ptl_m_          = source.ptl_m_;
  destination.ptl_y_          = source.ptl_y_;
  destination.ptm_low_        = source.ptm_low_;
  destination.ptm_low_rng_    = source.ptm_low_rng_;
  destination.ptm_low_st_     = source.ptm_low_st_;
  destination.ptm_high_       = source.ptm_high_;
  destination.ptm_high_rng_   = source.ptm_high_rng_;
  destination.ptm_high_st_    = source.ptm_high_st_;
  destination.brl_            = source.brl_;
  destination.brl_r_          = source.brl_r_;
  destination.brl_g_          = source.brl_g_;
  destination.brl_b_          = source.brl_b_;
  destination.brl_rng_        = source.brl_rng_;
  destination.brl_st_         = source.brl_st_;
  destination.brlp_           = source.brlp_;
  destination.brlp_r_         = source.brlp_r_;
  destination.brlp_g_         = source.brlp_g_;
  destination.brlp_b_         = source.brlp_b_;
  destination.hc_r_           = source.hc_r_;
  destination.hc_r_rng_       = source.hc_r_rng_;
  destination.hs_r_           = source.hs_r_;
  destination.hs_r_rng_       = source.hs_r_rng_;
  destination.hs_g_           = source.hs_g_;
  destination.hs_g_rng_       = source.hs_g_rng_;
  destination.hs_b_           = source.hs_b_;
  destination.hs_b_rng_       = source.hs_b_rng_;
  destination.hs_c_           = source.hs_c_;
  destination.hs_c_rng_       = source.hs_c_rng_;
  destination.hs_m_           = source.hs_m_;
  destination.hs_m_rng_       = source.hs_m_rng_;
  destination.hs_y_           = source.hs_y_;
  destination.hs_y_rng_       = source.hs_y_rng_;
  destination.ts_x1_          = source.ts_x1_;
  destination.ts_y1_          = source.ts_y1_;
  destination.ts_x0_          = source.ts_x0_;
  destination.ts_y0_          = source.ts_y0_;
  destination.ts_s0_          = source.ts_s0_;
  destination.ts_p_           = source.ts_p_;
  destination.ts_s10_         = source.ts_s10_;
  destination.ts_m1_          = source.ts_m1_;
  destination.ts_m2_          = source.ts_m2_;
  destination.ts_s_           = source.ts_s_;
  destination.ts_dsc_         = source.ts_dsc_;
  destination.pt_cmp_Lf_      = source.pt_cmp_Lf_;
  destination.s_Lp100_        = source.s_Lp100_;
  destination.ts_s1_          = source.ts_s1_;
}

}  // namespace

auto ResolveOpenClDrtParams(const nlohmann::json& odt_json)
    -> OpenCL::Pipeline::OpenClToOutputParams {
  ODT_Op         descriptor(nlohmann::json{{"odt", odt_json}});
  OperatorParams cpu_params;
  descriptor.SetGlobalParams(cpu_params);
  const auto&                            cpu = cpu_params.to_output_params_;

  OpenCL::Pipeline::OpenClToOutputParams gpu;
  gpu.method_ = static_cast<std::int32_t>(cpu.method_);
  gpu.eotf_   = static_cast<std::int32_t>(cpu.eotf_);
  Copy33(cpu.limit_to_display_matx_, gpu.limit_to_display_matx);
  gpu.display_linear_scale_ = cpu.display_linear_scale_;

  CopyAces(cpu.aces_params_, gpu.aces_params_);
  if (cpu.method_ == ColorUtils::ODTMethod::ACES_2_0) {
    if (!cpu.aces_params_.table_reach_M_ || !cpu.aces_params_.table_hues_ ||
        !cpu.aces_params_.table_upper_hull_gammas_ || !cpu.aces_params_.table_gamut_cusps_) {
      throw std::runtime_error("ResolveOpenClDrtParams: ACES 2.0 tables were not resolved");
    }
    constexpr auto table_size = OpenCL::Pipeline::kOpenClAcesOdtTableSize;
    std::copy_n(cpu.aces_params_.table_reach_M_->data(), table_size,
                gpu.aces_params_.table_reach_M_);
    std::copy_n(cpu.aces_params_.table_hues_->data(), table_size, gpu.aces_params_.table_hues_);
    std::copy_n(cpu.aces_params_.table_upper_hull_gammas_->data(), table_size,
                gpu.aces_params_.table_upper_hull_gamma_);
    for (int index = 0; index < table_size; ++index) {
      const auto& cusp = (*cpu.aces_params_.table_gamut_cusps_)[static_cast<std::size_t>(index)];
      gpu.aces_params_.table_gamut_cusps_[index][0] = cusp(0);
      gpu.aces_params_.table_gamut_cusps_[index][1] = cusp(1);
      gpu.aces_params_.table_gamut_cusps_[index][2] = cusp(2);
      gpu.aces_params_.table_gamut_cusps_[index][3] = 0.0f;
    }
  }
  CopyOpenDrt(cpu.open_drt_params_, gpu.open_drt_params_);
  return gpu;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
