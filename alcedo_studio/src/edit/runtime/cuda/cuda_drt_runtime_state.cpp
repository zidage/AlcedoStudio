//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_drt_runtime_state.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace alcedo {
namespace {

void Copy33(const cv::Matx33f& m, float out[9]) {
  out[0] = m(0, 0);
  out[1] = m(0, 1);
  out[2] = m(0, 2);
  out[3] = m(1, 0);
  out[4] = m(1, 1);
  out[5] = m(1, 2);
  out[6] = m(2, 0);
  out[7] = m(2, 1);
  out[8] = m(2, 2);
}

void CopyJmh(const ColorUtils::JMhParams& src, CudaDrtJmhParams& dst) {
  Copy33(src.MATRIX_RGB_to_CAM16_c_, dst.MATRIX_RGB_to_CAM16_c_);
  Copy33(src.MATRIX_CAM16_c_to_RGB_, dst.MATRIX_CAM16_c_to_RGB_);
  Copy33(src.MATRIX_cone_response_to_Aab_, dst.MATRIX_cone_response_to_Aab_);
  Copy33(src.MATRIX_Aab_to_cone_response_, dst.MATRIX_Aab_to_cone_response_);
  dst.F_L_n_     = src.F_L_n_;
  dst.cz_        = src.cz_;
  dst.inv_cz_    = src.inv_cz_;
  dst.A_w_J_     = (src.inv_A_w_J_ != 0.f) ? (1.f / src.inv_A_w_J_) : 0.f;
  dst.inv_A_w_J_ = src.inv_A_w_J_;
}

void CopyAcesScalars(const ColorUtils::ODTParams& src, CudaDrtAcesParams& dst) {
  dst.peak_luminance_               = src.peak_luminance_;
  dst.limit_J_max                   = src.limit_J_max_;
  dst.model_gamma_inv               = src.model_gamma_inv_;
  dst.ts_.n_                        = src.ts_params_.n_;
  dst.ts_.n_r_                      = src.ts_params_.n_r_;
  dst.ts_.g_                        = src.ts_params_.g_;
  dst.ts_.t_1_                      = src.ts_params_.t_1_;
  dst.ts_.c_t_                      = src.ts_params_.c_t_;
  dst.ts_.s_2_                      = src.ts_params_.s_2_;
  dst.ts_.u_2_                      = src.ts_params_.u_2_;
  dst.ts_.m_2_                      = src.ts_params_.m_2_;
  dst.ts_.forward_limit_            = src.ts_params_.forward_limit_;
  dst.ts_.inverse_limit_            = src.ts_params_.inverse_limit_;
  dst.ts_.log_peak_                 = src.ts_params_.log_peak_;
  dst.sat                           = src.sat_;
  dst.sat_thr                       = src.sat_thr_;
  dst.compr                         = src.compr_;
  dst.chroma_compress_scale         = src.chroma_compress_scale_;
  dst.mid_J                         = src.mid_J_;
  dst.focus_dist                    = src.focus_dist_;
  dst.lower_hull_gamma_inv          = src.lower_hull_gamma_inv_;
  dst.hue_linearity_search_range[0] = static_cast<int>(src.hue_linearity_search_range_(0));
  dst.hue_linearity_search_range[1] = static_cast<int>(src.hue_linearity_search_range_(1));
  CopyJmh(src.input_params_, dst.input_params_);
  CopyJmh(src.reach_params_, dst.reach_params_);
  CopyJmh(src.limit_params_, dst.limit_params_);
}

template <class T, class Host>
void UploadIfChanged(const std::shared_ptr<Host>& host, CudaDrtTable1D<T>& table,
                     std::uintptr_t& uploaded_id, const T* data, std::size_t count) {
  const auto id = reinterpret_cast<std::uintptr_t>(host.get());
  if (id == uploaded_id) {
    return;
  }
  table.Reset();
  table       = CreateCudaDrtTable<T>(data, count);
  uploaded_id = id;
}

void UploadAcesTables(const ColorUtils::ODTParams& src, CudaDrtAcesParams& dst) {
  UploadIfChanged(src.table_reach_M_, dst.table_reach_M_, dst.host_table_reach_M_id_,
                  src.table_reach_M_->data(), TOTAL_TABLE_SIZE);
  UploadIfChanged(src.table_hues_, dst.table_hues_, dst.host_table_hues_id_,
                  src.table_hues_->data(), TOTAL_TABLE_SIZE);
  if (reinterpret_cast<std::uintptr_t>(src.table_gamut_cusps_.get()) !=
      dst.host_table_gamut_cusps_id_) {
    std::vector<float4> packed(TOTAL_TABLE_SIZE);
    for (std::size_t i = 0; i < TOTAL_TABLE_SIZE; ++i) {
      const auto& cusp = (*src.table_gamut_cusps_)[i];
      packed[i]        = make_float4(cusp(0), cusp(1), cusp(2), 0.f);
    }
    UploadIfChanged(src.table_gamut_cusps_, dst.table_gamut_cusps_, dst.host_table_gamut_cusps_id_,
                    packed.data(), packed.size());
  }
  UploadIfChanged(src.table_upper_hull_gammas_, dst.table_upper_hull_gamma_,
                  dst.host_table_upper_hull_gamma_id_, src.table_upper_hull_gammas_->data(),
                  TOTAL_TABLE_SIZE);
}

void CopyOpenDrt(const ColorUtils::OpenDRTParams& src, CudaDrtOpenDrtParams& dst) {
  dst.tn_hcon_enable_ = src.tn_hcon_enable_;
  dst.tn_lcon_enable_ = src.tn_lcon_enable_;
  dst.pt_enable_      = src.pt_enable_;
  dst.ptl_enable_     = src.ptl_enable_;
  dst.ptm_enable_     = src.ptm_enable_;
  dst.brl_enable_     = src.brl_enable_;
  dst.brlp_enable_    = src.brlp_enable_;
  dst.hc_enable_      = src.hc_enable_;
  dst.hs_rgb_enable_  = src.hs_rgb_enable_;
  dst.hs_cmy_enable_  = src.hs_cmy_enable_;
  dst.creative_white_ = src.creative_white_;
  dst.surround_       = src.surround_;
  dst.clamp_          = src.clamp_;
  dst.display_gamut_  = src.display_gamut_;
  dst.display_eotf_   = src.display_eotf_;
  dst.tn_con_         = src.tn_con_;
  dst.tn_sh_          = src.tn_sh_;
  dst.tn_toe_         = src.tn_toe_;
  dst.tn_off_         = src.tn_off_;
  dst.tn_hcon_        = src.tn_hcon_;
  dst.tn_hcon_pv_     = src.tn_hcon_pv_;
  dst.tn_hcon_st_     = src.tn_hcon_st_;
  dst.tn_lcon_        = src.tn_lcon_;
  dst.tn_lcon_w_      = src.tn_lcon_w_;
  dst.cwp_lm_         = src.cwp_lm_;
  dst.rs_sa_          = src.rs_sa_;
  dst.rs_rw_          = src.rs_rw_;
  dst.rs_bw_          = src.rs_bw_;
  dst.pt_lml_         = src.pt_lml_;
  dst.pt_lml_r_       = src.pt_lml_r_;
  dst.pt_lml_g_       = src.pt_lml_g_;
  dst.pt_lml_b_       = src.pt_lml_b_;
  dst.pt_lmh_         = src.pt_lmh_;
  dst.pt_lmh_r_       = src.pt_lmh_r_;
  dst.pt_lmh_b_       = src.pt_lmh_b_;
  dst.ptl_c_          = src.ptl_c_;
  dst.ptl_m_          = src.ptl_m_;
  dst.ptl_y_          = src.ptl_y_;
  dst.ptm_low_        = src.ptm_low_;
  dst.ptm_low_rng_    = src.ptm_low_rng_;
  dst.ptm_low_st_     = src.ptm_low_st_;
  dst.ptm_high_       = src.ptm_high_;
  dst.ptm_high_rng_   = src.ptm_high_rng_;
  dst.ptm_high_st_    = src.ptm_high_st_;
  dst.brl_            = src.brl_;
  dst.brl_r_          = src.brl_r_;
  dst.brl_g_          = src.brl_g_;
  dst.brl_b_          = src.brl_b_;
  dst.brl_rng_        = src.brl_rng_;
  dst.brl_st_         = src.brl_st_;
  dst.brlp_           = src.brlp_;
  dst.brlp_r_         = src.brlp_r_;
  dst.brlp_g_         = src.brlp_g_;
  dst.brlp_b_         = src.brlp_b_;
  dst.hc_r_           = src.hc_r_;
  dst.hc_r_rng_       = src.hc_r_rng_;
  dst.hs_r_           = src.hs_r_;
  dst.hs_r_rng_       = src.hs_r_rng_;
  dst.hs_g_           = src.hs_g_;
  dst.hs_g_rng_       = src.hs_g_rng_;
  dst.hs_b_           = src.hs_b_;
  dst.hs_b_rng_       = src.hs_b_rng_;
  dst.hs_c_           = src.hs_c_;
  dst.hs_c_rng_       = src.hs_c_rng_;
  dst.hs_m_           = src.hs_m_;
  dst.hs_m_rng_       = src.hs_m_rng_;
  dst.hs_y_           = src.hs_y_;
  dst.hs_y_rng_       = src.hs_y_rng_;
  dst.ts_x1_          = src.ts_x1_;
  dst.ts_y1_          = src.ts_y1_;
  dst.ts_x0_          = src.ts_x0_;
  dst.ts_y0_          = src.ts_y0_;
  dst.ts_s0_          = src.ts_s0_;
  dst.ts_p_           = src.ts_p_;
  dst.ts_s10_         = src.ts_s10_;
  dst.ts_m1_          = src.ts_m1_;
  dst.ts_m2_          = src.ts_m2_;
  dst.ts_s_           = src.ts_s_;
  dst.ts_dsc_         = src.ts_dsc_;
  dst.pt_cmp_Lf_      = src.pt_cmp_Lf_;
  dst.s_Lp100_        = src.s_Lp100_;
  dst.ts_s1_          = src.ts_s1_;
}

}  // namespace

CudaDrtRuntimeState::~CudaDrtRuntimeState() { params_.Reset(); }

auto CudaDrtRuntimeState::Pack(const ColorUtils::TO_OUTPUT_Params& resolved)
    -> const CudaDrtGpuParams& {
  if (resolved.method_ == ColorUtils::ODTMethod::ACES_2_0) {
    const auto& aces = resolved.aces_params_;
    if (!aces.table_reach_M_ || !aces.table_hues_ || !aces.table_upper_hull_gammas_ ||
        !aces.table_gamut_cusps_) {
      std::ostringstream message;
      message << "CudaDrtRuntimeState: ACES ODT tables are not initialized:";
      if (!aces.table_reach_M_) message << " table_reach_M_";
      if (!aces.table_hues_) message << " table_hues_";
      if (!aces.table_upper_hull_gammas_) message << " table_upper_hull_gammas_";
      if (!aces.table_gamut_cusps_) message << " table_gamut_cusps_";
      throw std::runtime_error(message.str());
    }
  }

  Copy33(resolved.limit_to_display_matx_, params_.limit_to_display_matx);
  params_.eotf                  = static_cast<CudaDrtEotf>(static_cast<int>(resolved.eotf_));
  params_.method_               = static_cast<CudaDrtMethod>(static_cast<int>(resolved.method_));
  params_.display_linear_scale_ = resolved.display_linear_scale_;

  if (resolved.method_ == ColorUtils::ODTMethod::ACES_2_0) {
    params_.open_drt_params_ = {};
    CopyAcesScalars(resolved.aces_params_, params_.aces_params_);
    UploadAcesTables(resolved.aces_params_, params_.aces_params_);
  } else {
    params_.aces_params_.Reset();
    params_.aces_params_ = {};
    CopyOpenDrt(resolved.open_drt_params_, params_.open_drt_params_);
  }
  return params_;
}

}  // namespace alcedo
