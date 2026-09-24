//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "edit/operators/op_base.hpp"

namespace alcedo {

enum class GPU_EOTF : int {
  LINEAR    = 0,
  ST2084    = 1,
  HLG       = 2,
  GAMMA_2_6 = 3,
  BT1886    = 4,
  GAMMA_2_2 = 5,
  GAMMA_1_8 = 6,
};

enum class GPU_ODTMethod : int {
  ACES_2_0 = 0,
  OPEN_DRT = 1,
};

struct Fused_JMhParams {
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

struct Fused_TSParams {
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

struct Fused_ODTParams {
  float           peak_luminance_               = 100.0f;
  Fused_JMhParams input_params_                 = {};
  Fused_JMhParams reach_params_                 = {};
  Fused_JMhParams limit_params_                 = {};
  Fused_TSParams  ts_                           = {};
  float           limit_J_max                   = 0.0f;
  float           model_gamma_inv               = 0.0f;
  float           sat                           = 0.0f;
  float           sat_thr                       = 0.0f;
  float           compr                         = 0.0f;
  float           chroma_compress_scale         = 0.0f;
  float           mid_J                         = 0.0f;
  float           focus_dist                    = 0.0f;
  float           lower_hull_gamma_inv          = 0.0f;
  int             hue_linearity_search_range[2] = {0, 1};
};

struct Fused_OpenDRTParams {
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

struct Fused_TO_OUTPUT_Params {
  GPU_ODTMethod       method_                  = GPU_ODTMethod::OPEN_DRT;
  Fused_ODTParams     aces_params_             = {};
  Fused_OpenDRTParams open_drt_params_         = {};
  float               limit_to_display_matx[9] = {};
  float               display_linear_scale_    = 1.0f;
  GPU_EOTF            eotf                     = GPU_EOTF::LINEAR;
};

struct FusedOperatorParams {
  using ToneMappingParams                                  = OperatorParams::ToneMappingParams;
  using FilmGrainParams                                    = OperatorParams::FilmGrainParams;
  using HalationParams                                     = OperatorParams::HalationParams;

  ToneMappingParams tone_mapping_                          = {};
  FilmGrainParams   film_grain_                            = {};
  HalationParams    halation_                              = {};

  bool              exposure_enabled_                      = true;
  float             exposure_offset_                       = 0.0f;

  bool              contrast_enabled_                      = true;
  float             contrast_scale_                        = 0.0f;

  bool              shadows_enabled_                       = true;
  float             shadows_offset_                        = 0.0f;
  float             shadows_x0_                            = 0.0f;
  float             shadows_x1_                            = 0.25f;
  float             shadows_y0_                            = 0.0f;
  float             shadows_y1_                            = 0.25f;
  float             shadows_m0_                            = 0.0f;
  float             shadows_m1_                            = 1.0f;
  float             shadows_dx_                            = 0.25f;

  bool              highlights_enabled_                    = true;
  float             highlights_k_                          = 0.2f;
  float             highlights_offset_                     = 0.0f;
  float             highlights_slope_range_                = 0.8f;
  float             highlights_m0_                         = 1.0f;
  float             highlights_m1_                         = 1.0f;
  float             highlights_x0_                         = 0.2f;
  float             highlights_y0_                         = 0.2f;
  float             highlights_y1_                         = 1.0f;
  float             highlights_dx_                         = 0.8f;

  bool              shared_tone_curve_enabled_             = false;
  bool              shared_tone_curve_apply_in_shadows_    = false;
  bool              shared_tone_curve_apply_in_highlights_ = false;
  int               shared_tone_curve_ctrl_pts_size_       = 0;
  float shared_tone_curve_ctrl_pts_x_[OperatorParams::kSharedToneCurveControlPointCount] = {};
  float shared_tone_curve_ctrl_pts_y_[OperatorParams::kSharedToneCurveControlPointCount] = {};
  float shared_tone_curve_h_[OperatorParams::kSharedToneCurveControlPointCount - 1]      = {};
  float shared_tone_curve_m_[OperatorParams::kSharedToneCurveControlPointCount]          = {};

  bool  hs_local_tone_enabled_                                                           = true;
  float hs_base_radius_                                                                  = 18.0f;
  int   hs_base_gaussian_tap_count_                                                      = 0;
  float hs_base_gaussian_weights_[OperatorParams::kDetailMaxGaussianTapCount]            = {};
  float hs_shadow_log_pivot_                                                             = -2.45f;
  float hs_shadow_log_width_                                                             = 1.35f;
  float hs_highlight_log_pivot_                                                          = -0.20f;
  float hs_highlight_log_width_                                                          = 1.15f;
  std::uint64_t hs_mask_base_cache_key_                                                  = 0;
  std::uint64_t render_source_cache_key_                                                 = 0;
  int           render_frame_role_            = OperatorParams::kRenderFrameRoleInteractivePrimary;
  bool          render_hs_can_seed_reference_ = false;
  int           render_hs_reference_max_long_edge_          = 2048;
  bool          render_hs_preserve_source_detail_           = false;
  bool          render_roi_enabled_                         = false;
  int           render_roi_x_                               = 0;
  int           render_roi_y_                               = 0;
  float         render_roi_scale_x_                         = 1.0f;
  float         render_roi_scale_y_                         = 1.0f;
  int           render_roi_reference_width_                 = 0;
  int           render_roi_reference_height_                = 0;
  float         render_output_scale_x_                      = 1.0f;
  float         render_output_scale_y_                      = 1.0f;

  bool          white_enabled_                              = true;
  float         white_point_                                = 1.0f;

  bool          black_enabled_                              = true;
  float         black_point_                                = 0.0f;

  float         slope_                                      = 1.0f;

  bool          hls_enabled_                                = true;
  float         target_hls_[3]                              = {0.0f, 0.5f, 1.0f};
  float         hls_adjustment_[3]                          = {0.0f, 0.0f, 0.0f};
  float         hue_range_                                  = 45.0f;
  float         lightness_range_                            = 0.1f;
  float         saturation_range_                           = 0.1f;
  int           hls_profile_count_                          = OperatorParams::kHlsProfileCount;
  float hls_profile_hues_[OperatorParams::kHlsProfileCount] = {0.0f,   45.0f,  90.0f,  135.0f,
                                                               180.0f, 225.0f, 270.0f, 315.0f};
  float hls_profile_adjustments_[OperatorParams::kHlsProfileCount][3] = {};
  float hls_profile_hue_ranges_[OperatorParams::kHlsProfileCount]     = {45.0f, 45.0f, 45.0f, 45.0f,
                                                                         45.0f, 45.0f, 45.0f, 45.0f};

  bool  saturation_enabled_                                           = true;
  float saturation_offset_                                            = 1.0f;

  bool  tint_enabled_                                                 = true;
  float tint_offset_                                                  = 0.0f;

  bool  vibrance_enabled_                                             = true;
  float vibrance_offset_                                              = 0.0f;

  bool  to_ws_enabled_                                                = true;

  bool  color_temp_enabled_                                           = true;
  int   color_temp_mode_                                              = 0;
  float color_temp_custom_cct_                                        = 6500.0f;
  float color_temp_custom_tint_                                       = 0.0f;
  float color_temp_resolved_cct_                                      = 6500.0f;
  float color_temp_resolved_tint_                                     = 0.0f;
  float color_temp_resolved_xy_[2]                                    = {0.3127f, 0.3290f};

  bool  raw_runtime_valid_                                            = false;
  int   raw_decode_input_space_                                       = 0;
  float raw_cam_mul_[3]                                               = {1.0f, 1.0f, 1.0f};
  float raw_pre_mul_[3]                                               = {1.0f, 1.0f, 1.0f};
  float raw_cam_xyz_[9]                                               = {};

  bool  color_temp_matrices_valid_                                    = false;
  float color_temp_cam_to_xyz_[9]          = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float color_temp_cam_to_xyz_d50_[9]      = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float color_temp_xyz_d50_to_ap1_[9]      = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float color_temp_cam_to_ap1_[9]          = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

  bool  lmt_enabled_                       = false;

  bool  to_output_enabled_                 = true;
  Fused_TO_OUTPUT_Params to_output_params_ = {};

  static constexpr int   kMaxCurveControlPoints                    = 32;
  bool                   curve_enabled_                            = true;
  int                    curve_ctrl_pts_size_                      = 0;
  float                  curve_ctrl_pts_x_[kMaxCurveControlPoints] = {};
  float                  curve_ctrl_pts_y_[kMaxCurveControlPoints] = {};
  float                  curve_h_[kMaxCurveControlPoints - 1]      = {};
  float                  curve_m_[kMaxCurveControlPoints]          = {};

  bool                   clarity_enabled_                          = true;
  float                  clarity_offset_                           = 0.0f;
  float                  clarity_radius_                           = 5.0f;
  int                    clarity_gaussian_tap_count_               = 0;
  float                  clarity_gaussian_weights_[OperatorParams::kDetailMaxGaussianTapCount] = {};

  bool                   sharpen_enabled_            = true;
  float                  sharpen_offset_             = 0.0f;
  float                  sharpen_radius_             = 3.0f;
  float                  sharpen_threshold_          = 0.0f;
  int                    sharpen_gaussian_tap_count_ = 0;
  float                  sharpen_gaussian_weights_[OperatorParams::kDetailMaxGaussianTapCount] = {};

  bool                   color_wheel_enabled_    = true;
  float                  lift_color_offset_[3]   = {0.0f, 0.0f, 0.0f};
  float                  lift_luminance_offset_  = 0.0f;
  float                  gamma_color_offset_[3]  = {1.0f, 1.0f, 1.0f};
  float                  gamma_luminance_offset_ = 0.0f;
  float                  gain_color_offset_[3]   = {1.0f, 1.0f, 1.0f};
  float                  gain_luminance_offset_  = 0.0f;
};

class FusedParamsConverter {
 public:
  static auto ConvertFromCPU(OperatorParams&            cpu_params,
                             const FusedOperatorParams& orig_params = {}) -> FusedOperatorParams {
    FusedOperatorParams fused                 = orig_params;

    fused.film_grain_                         = cpu_params.film_grain_;
    fused.halation_                           = cpu_params.halation_;

    fused.exposure_enabled_                   = cpu_params.exposure_enabled_;
    fused.exposure_offset_                    = cpu_params.exposure_offset_;
    fused.contrast_enabled_                   = cpu_params.contrast_enabled_;
    fused.contrast_scale_                     = cpu_params.contrast_scale_;
    fused.shadows_enabled_                    = cpu_params.shadows_enabled_;
    fused.shadows_offset_                     = cpu_params.shadows_offset_;
    fused.shadows_x0_                         = cpu_params.shadows_x0_;
    fused.shadows_x1_                         = cpu_params.shadows_x1_;
    fused.shadows_y0_                         = cpu_params.shadows_y0_;
    fused.shadows_y1_                         = cpu_params.shadows_y1_;
    fused.shadows_m0_                         = cpu_params.shadows_m0_;
    fused.shadows_m1_                         = cpu_params.shadows_m1_;
    fused.shadows_dx_                         = cpu_params.shadows_dx_;
    fused.highlights_enabled_                 = cpu_params.highlights_enabled_;
    fused.highlights_k_                       = cpu_params.highlights_k_;
    fused.highlights_offset_                  = cpu_params.highlights_offset_;
    fused.highlights_slope_range_             = cpu_params.highlights_slope_range_;
    fused.highlights_m0_                      = cpu_params.highlights_m0_;
    fused.highlights_m1_                      = cpu_params.highlights_m1_;
    fused.highlights_x0_                      = cpu_params.highlights_x0_;
    fused.highlights_y0_                      = cpu_params.highlights_y0_;
    fused.highlights_y1_                      = cpu_params.highlights_y1_;
    fused.highlights_dx_                      = cpu_params.highlights_dx_;
    fused.shared_tone_curve_enabled_          = cpu_params.shared_tone_curve_enabled_;
    fused.shared_tone_curve_apply_in_shadows_ = cpu_params.shared_tone_curve_apply_in_shadows_;
    fused.shared_tone_curve_apply_in_highlights_ =
        cpu_params.shared_tone_curve_apply_in_highlights_;
    fused.shared_tone_curve_ctrl_pts_size_ = cpu_params.shared_tone_curve_ctrl_pts_size_;
    for (int i = 0; i < OperatorParams::kSharedToneCurveControlPointCount; ++i) {
      fused.shared_tone_curve_ctrl_pts_x_[i] = cpu_params.shared_tone_curve_ctrl_pts_x_[i];
      fused.shared_tone_curve_ctrl_pts_y_[i] = cpu_params.shared_tone_curve_ctrl_pts_y_[i];
      fused.shared_tone_curve_m_[i]          = cpu_params.shared_tone_curve_m_[i];
      if (i < OperatorParams::kSharedToneCurveControlPointCount - 1) {
        fused.shared_tone_curve_h_[i] = cpu_params.shared_tone_curve_h_[i];
      }
    }
    fused.tone_mapping_                         = cpu_params.tone_mapping_;
    fused.tone_mapping_.base_cache_key_         = BuildHsMaskBaseCacheKey(cpu_params);
    fused.tone_mapping_.preserve_source_detail_ = cpu_params.render_hs_preserve_source_detail_;
    fused.tone_mapping_.roi_enabled_            = cpu_params.render_roi_enabled_;
    fused.tone_mapping_.roi_x_                  = cpu_params.render_roi_x_;
    fused.tone_mapping_.roi_y_                  = cpu_params.render_roi_y_;
    fused.tone_mapping_.roi_scale_x_            = cpu_params.render_roi_scale_x_;
    fused.tone_mapping_.roi_scale_y_            = cpu_params.render_roi_scale_y_;
    fused.tone_mapping_.roi_reference_width_    = cpu_params.render_roi_reference_width_;
    fused.tone_mapping_.roi_reference_height_   = cpu_params.render_roi_reference_height_;

    fused.hs_local_tone_enabled_                = fused.tone_mapping_.local_tone_enabled_;
    fused.hs_base_radius_                       = fused.tone_mapping_.local_radius_;
    fused.hs_base_gaussian_tap_count_ = std::clamp(fused.tone_mapping_.base_gaussian_tap_count_, 0,
                                                   OperatorParams::kDetailMaxGaussianTapCount);
    for (int i = 0; i < OperatorParams::kDetailMaxGaussianTapCount; ++i) {
      fused.hs_base_gaussian_weights_[i] = fused.tone_mapping_.base_gaussian_weights_[i];
    }
    fused.hs_shadow_log_pivot_               = fused.tone_mapping_.shadow_log_pivot_;
    fused.hs_shadow_log_width_               = fused.tone_mapping_.shadow_log_width_;
    fused.hs_highlight_log_pivot_            = fused.tone_mapping_.highlight_log_pivot_;
    fused.hs_highlight_log_width_            = fused.tone_mapping_.highlight_log_width_;
    fused.hs_mask_base_cache_key_            = fused.tone_mapping_.base_cache_key_;
    fused.render_source_cache_key_           = cpu_params.render_source_cache_key_;
    fused.render_frame_role_                 = cpu_params.render_frame_role_;
    fused.render_hs_can_seed_reference_      = cpu_params.render_hs_can_seed_reference_;
    fused.render_hs_reference_max_long_edge_ = cpu_params.render_hs_reference_max_long_edge_;
    fused.render_hs_preserve_source_detail_  = cpu_params.render_hs_preserve_source_detail_;
    fused.render_roi_enabled_                = cpu_params.render_roi_enabled_;
    fused.render_roi_x_                      = cpu_params.render_roi_x_;
    fused.render_roi_y_                      = cpu_params.render_roi_y_;
    fused.render_roi_scale_x_                = cpu_params.render_roi_scale_x_;
    fused.render_roi_scale_y_                = cpu_params.render_roi_scale_y_;
    fused.render_roi_reference_width_        = cpu_params.render_roi_reference_width_;
    fused.render_roi_reference_height_       = cpu_params.render_roi_reference_height_;
    fused.render_output_scale_x_             = cpu_params.render_output_scale_x_;
    fused.render_output_scale_y_             = cpu_params.render_output_scale_y_;
    fused.white_enabled_                     = cpu_params.white_enabled_;
    fused.white_point_                       = cpu_params.white_point_;
    fused.black_enabled_                     = cpu_params.black_enabled_;
    fused.black_point_                       = cpu_params.black_point_;
    fused.slope_                             = cpu_params.slope_;
    fused.hls_enabled_                       = cpu_params.hls_enabled_;
    for (int i = 0; i < 3; ++i) {
      fused.target_hls_[i]     = cpu_params.target_hls_[i];
      fused.hls_adjustment_[i] = cpu_params.hls_adjustment_[i];
    }
    fused.hue_range_        = cpu_params.hue_range_;
    fused.lightness_range_  = cpu_params.lightness_range_;
    fused.saturation_range_ = cpu_params.saturation_range_;
    fused.hls_profile_count_ =
        std::clamp(cpu_params.hls_profile_count_, 1, OperatorParams::kHlsProfileCount);
    for (int i = 0; i < OperatorParams::kHlsProfileCount; ++i) {
      fused.hls_profile_hues_[i]           = cpu_params.hls_profile_hues_[i];
      fused.hls_profile_hue_ranges_[i]     = cpu_params.hls_profile_hue_ranges_[i];
      fused.hls_profile_adjustments_[i][0] = cpu_params.hls_profile_adjustments_[i][0];
      fused.hls_profile_adjustments_[i][1] = cpu_params.hls_profile_adjustments_[i][1];
      fused.hls_profile_adjustments_[i][2] = cpu_params.hls_profile_adjustments_[i][2];
    }
    fused.saturation_enabled_        = cpu_params.saturation_enabled_;
    fused.saturation_offset_         = cpu_params.saturation_offset_;
    fused.tint_enabled_              = cpu_params.tint_enabled_;
    fused.tint_offset_               = cpu_params.tint_offset_;
    fused.vibrance_enabled_          = cpu_params.vibrance_enabled_;
    fused.vibrance_offset_           = cpu_params.vibrance_offset_;
    fused.to_ws_enabled_             = cpu_params.to_ws_enabled_;

    fused.color_temp_enabled_        = cpu_params.color_temp_enabled_;
    fused.color_temp_mode_           = static_cast<int>(cpu_params.color_temp_mode_);
    fused.color_temp_custom_cct_     = cpu_params.color_temp_custom_cct_;
    fused.color_temp_custom_tint_    = cpu_params.color_temp_custom_tint_;
    fused.color_temp_resolved_cct_   = cpu_params.color_temp_resolved_cct_;
    fused.color_temp_resolved_tint_  = cpu_params.color_temp_resolved_tint_;
    fused.color_temp_resolved_xy_[0] = cpu_params.color_temp_resolved_xy_[0];
    fused.color_temp_resolved_xy_[1] = cpu_params.color_temp_resolved_xy_[1];
    fused.raw_runtime_valid_         = cpu_params.raw_runtime_valid_;
    fused.raw_decode_input_space_    = static_cast<int>(cpu_params.raw_decode_input_space_);
    for (int i = 0; i < 3; ++i) {
      fused.raw_cam_mul_[i] = cpu_params.raw_cam_mul_[i];
      fused.raw_pre_mul_[i] = cpu_params.raw_pre_mul_[i];
    }
    for (int i = 0; i < 9; ++i) {
      fused.raw_cam_xyz_[i]               = cpu_params.raw_cam_xyz_[i];
      fused.color_temp_cam_to_xyz_[i]     = cpu_params.color_temp_cam_to_xyz_[i];
      fused.color_temp_cam_to_xyz_d50_[i] = cpu_params.color_temp_cam_to_xyz_d50_[i];
      fused.color_temp_xyz_d50_to_ap1_[i] = cpu_params.color_temp_xyz_d50_to_ap1_[i];
      fused.color_temp_cam_to_ap1_[i]     = cpu_params.color_temp_cam_to_ap1_[i];
    }
    fused.color_temp_matrices_valid_ = cpu_params.color_temp_matrices_valid_;
    fused.lmt_enabled_               = cpu_params.lmt_enabled_;
    fused.to_output_enabled_         = cpu_params.to_output_enabled_;

    fused.curve_enabled_             = cpu_params.curve_enabled_;
    const size_t curve_pts_count     = cpu_params.curve_ctrl_pts_.size();
    if (curve_pts_count > static_cast<size_t>(FusedOperatorParams::kMaxCurveControlPoints)) {
      std::ostringstream oss;
      oss << "FusedParamsConverter: curve has " << curve_pts_count
          << " control points, but fused max is " << FusedOperatorParams::kMaxCurveControlPoints
          << ".";
      throw std::runtime_error(oss.str());
    }
    if (curve_pts_count > 0 && cpu_params.curve_m_.size() < curve_pts_count) {
      throw std::runtime_error(
          "FusedParamsConverter: curve_m_ is smaller than curve control-point count.");
    }
    if (curve_pts_count > 1 && cpu_params.curve_h_.size() < (curve_pts_count - 1)) {
      throw std::runtime_error(
          "FusedParamsConverter: curve_h_ is smaller than curve segment count.");
    }
    fused.curve_ctrl_pts_size_ = static_cast<int>(curve_pts_count);
    for (int i = 0; i < FusedOperatorParams::kMaxCurveControlPoints; ++i) {
      fused.curve_ctrl_pts_x_[i] = 0.0f;
      fused.curve_ctrl_pts_y_[i] = 0.0f;
      fused.curve_m_[i]          = 0.0f;
      if (i < FusedOperatorParams::kMaxCurveControlPoints - 1) {
        fused.curve_h_[i] = 0.0f;
      }
    }
    for (size_t i = 0; i < curve_pts_count; ++i) {
      fused.curve_ctrl_pts_x_[i] = cpu_params.curve_ctrl_pts_[i].x;
      fused.curve_ctrl_pts_y_[i] = cpu_params.curve_ctrl_pts_[i].y;
      fused.curve_m_[i]          = cpu_params.curve_m_[i];
    }
    for (size_t i = 0; i + 1 < curve_pts_count; ++i) {
      fused.curve_h_[i] = cpu_params.curve_h_[i];
    }

    fused.clarity_enabled_            = fused.tone_mapping_.clarity_enabled_;
    fused.clarity_offset_             = fused.tone_mapping_.clarity_amount_;
    fused.clarity_radius_             = cpu_params.clarity_radius_;
    fused.clarity_gaussian_tap_count_ = std::clamp(cpu_params.clarity_gaussian_tap_count_, 0,
                                                   OperatorParams::kDetailMaxGaussianTapCount);
    for (int i = 0; i < OperatorParams::kDetailMaxGaussianTapCount; ++i) {
      fused.clarity_gaussian_weights_[i] = cpu_params.clarity_gaussian_weights_[i];
    }
    fused.sharpen_enabled_            = cpu_params.sharpen_enabled_;
    fused.sharpen_offset_             = cpu_params.sharpen_offset_;
    fused.sharpen_radius_             = cpu_params.sharpen_radius_;
    fused.sharpen_threshold_          = cpu_params.sharpen_threshold_;
    fused.sharpen_gaussian_tap_count_ = std::clamp(cpu_params.sharpen_gaussian_tap_count_, 0,
                                                   OperatorParams::kDetailMaxGaussianTapCount);
    for (int i = 0; i < OperatorParams::kDetailMaxGaussianTapCount; ++i) {
      fused.sharpen_gaussian_weights_[i] = cpu_params.sharpen_gaussian_weights_[i];
    }
    fused.color_wheel_enabled_ = cpu_params.color_wheel_enabled_;
    for (int i = 0; i < 3; ++i) {
      fused.lift_color_offset_[i]  = cpu_params.lift_color_offset_[i];
      fused.gamma_color_offset_[i] = cpu_params.gamma_color_offset_[i];
      fused.gain_color_offset_[i]  = cpu_params.gain_color_offset_[i];
    }
    fused.lift_luminance_offset_  = cpu_params.lift_luminance_offset_;
    fused.gamma_luminance_offset_ = cpu_params.gamma_luminance_offset_;
    fused.gain_luminance_offset_  = cpu_params.gain_luminance_offset_;

    auto copy33                   = [](const cv::Matx33f& m, float out[9]) {
      out[0] = m(0, 0);
      out[1] = m(0, 1);
      out[2] = m(0, 2);
      out[3] = m(1, 0);
      out[4] = m(1, 1);
      out[5] = m(1, 2);
      out[6] = m(2, 0);
      out[7] = m(2, 1);
      out[8] = m(2, 2);
    };
    auto copy_jmh = [&](const ColorUtils::JMhParams& src, Fused_JMhParams& dst) {
      copy33(src.MATRIX_RGB_to_CAM16_c_, dst.MATRIX_RGB_to_CAM16_c_);
      copy33(src.MATRIX_CAM16_c_to_RGB_, dst.MATRIX_CAM16_c_to_RGB_);
      copy33(src.MATRIX_cone_response_to_Aab_, dst.MATRIX_cone_response_to_Aab_);
      copy33(src.MATRIX_Aab_to_cone_response_, dst.MATRIX_Aab_to_cone_response_);
      dst.F_L_n_     = src.F_L_n_;
      dst.cz_        = src.cz_;
      dst.inv_cz_    = src.inv_cz_;
      dst.A_w_J_     = (src.inv_A_w_J_ != 0.f) ? (1.f / src.inv_A_w_J_) : 0.f;
      dst.inv_A_w_J_ = src.inv_A_w_J_;
    };

    const auto& to_output_cpu = cpu_params.to_output_params_;
    auto&       to_output     = fused.to_output_params_;
    to_output.method_         = static_cast<GPU_ODTMethod>(static_cast<int>(to_output_cpu.method_));
    to_output.eotf            = static_cast<GPU_EOTF>(static_cast<int>(to_output_cpu.eotf_));
    to_output.display_linear_scale_ = to_output_cpu.display_linear_scale_;
    copy33(to_output_cpu.limit_to_display_matx_, to_output.limit_to_display_matx);

    const auto& odt_cpu             = to_output_cpu.aces_params_;
    auto&       odt_fused           = to_output.aces_params_;
    odt_fused.peak_luminance_       = odt_cpu.peak_luminance_;
    odt_fused.limit_J_max           = odt_cpu.limit_J_max_;
    odt_fused.model_gamma_inv       = odt_cpu.model_gamma_inv_;
    odt_fused.ts_.n_                = odt_cpu.ts_params_.n_;
    odt_fused.ts_.n_r_              = odt_cpu.ts_params_.n_r_;
    odt_fused.ts_.g_                = odt_cpu.ts_params_.g_;
    odt_fused.ts_.t_1_              = odt_cpu.ts_params_.t_1_;
    odt_fused.ts_.c_t_              = odt_cpu.ts_params_.c_t_;
    odt_fused.ts_.s_2_              = odt_cpu.ts_params_.s_2_;
    odt_fused.ts_.u_2_              = odt_cpu.ts_params_.u_2_;
    odt_fused.ts_.m_2_              = odt_cpu.ts_params_.m_2_;
    odt_fused.ts_.forward_limit_    = odt_cpu.ts_params_.forward_limit_;
    odt_fused.ts_.inverse_limit_    = odt_cpu.ts_params_.inverse_limit_;
    odt_fused.ts_.log_peak_         = odt_cpu.ts_params_.log_peak_;
    odt_fused.sat                   = odt_cpu.sat_;
    odt_fused.sat_thr               = odt_cpu.sat_thr_;
    odt_fused.compr                 = odt_cpu.compr_;
    odt_fused.chroma_compress_scale = odt_cpu.chroma_compress_scale_;
    odt_fused.mid_J                 = odt_cpu.mid_J_;
    odt_fused.focus_dist            = odt_cpu.focus_dist_;
    odt_fused.lower_hull_gamma_inv  = odt_cpu.lower_hull_gamma_inv_;
    odt_fused.hue_linearity_search_range[0] =
        static_cast<int>(odt_cpu.hue_linearity_search_range_(0));
    odt_fused.hue_linearity_search_range[1] =
        static_cast<int>(odt_cpu.hue_linearity_search_range_(1));
    copy_jmh(odt_cpu.input_params_, odt_fused.input_params_);
    copy_jmh(odt_cpu.reach_params_, odt_fused.reach_params_);
    copy_jmh(odt_cpu.limit_params_, odt_fused.limit_params_);

    const auto& open_cpu = to_output_cpu.open_drt_params_;
    auto&       open     = to_output.open_drt_params_;
    open.tn_hcon_enable_ = open_cpu.tn_hcon_enable_;
    open.tn_lcon_enable_ = open_cpu.tn_lcon_enable_;
    open.pt_enable_      = open_cpu.pt_enable_;
    open.ptl_enable_     = open_cpu.ptl_enable_;
    open.ptm_enable_     = open_cpu.ptm_enable_;
    open.brl_enable_     = open_cpu.brl_enable_;
    open.brlp_enable_    = open_cpu.brlp_enable_;
    open.hc_enable_      = open_cpu.hc_enable_;
    open.hs_rgb_enable_  = open_cpu.hs_rgb_enable_;
    open.hs_cmy_enable_  = open_cpu.hs_cmy_enable_;
    open.creative_white_ = open_cpu.creative_white_;
    open.surround_       = open_cpu.surround_;
    open.clamp_          = open_cpu.clamp_;
    open.display_gamut_  = open_cpu.display_gamut_;
    open.display_eotf_   = open_cpu.display_eotf_;
    open.tn_con_         = open_cpu.tn_con_;
    open.tn_sh_          = open_cpu.tn_sh_;
    open.tn_toe_         = open_cpu.tn_toe_;
    open.tn_off_         = open_cpu.tn_off_;
    open.tn_hcon_        = open_cpu.tn_hcon_;
    open.tn_hcon_pv_     = open_cpu.tn_hcon_pv_;
    open.tn_hcon_st_     = open_cpu.tn_hcon_st_;
    open.tn_lcon_        = open_cpu.tn_lcon_;
    open.tn_lcon_w_      = open_cpu.tn_lcon_w_;
    open.cwp_lm_         = open_cpu.cwp_lm_;
    open.rs_sa_          = open_cpu.rs_sa_;
    open.rs_rw_          = open_cpu.rs_rw_;
    open.rs_bw_          = open_cpu.rs_bw_;
    open.pt_lml_         = open_cpu.pt_lml_;
    open.pt_lml_r_       = open_cpu.pt_lml_r_;
    open.pt_lml_g_       = open_cpu.pt_lml_g_;
    open.pt_lml_b_       = open_cpu.pt_lml_b_;
    open.pt_lmh_         = open_cpu.pt_lmh_;
    open.pt_lmh_r_       = open_cpu.pt_lmh_r_;
    open.pt_lmh_b_       = open_cpu.pt_lmh_b_;
    open.ptl_c_          = open_cpu.ptl_c_;
    open.ptl_m_          = open_cpu.ptl_m_;
    open.ptl_y_          = open_cpu.ptl_y_;
    open.ptm_low_        = open_cpu.ptm_low_;
    open.ptm_low_rng_    = open_cpu.ptm_low_rng_;
    open.ptm_low_st_     = open_cpu.ptm_low_st_;
    open.ptm_high_       = open_cpu.ptm_high_;
    open.ptm_high_rng_   = open_cpu.ptm_high_rng_;
    open.ptm_high_st_    = open_cpu.ptm_high_st_;
    open.brl_            = open_cpu.brl_;
    open.brl_r_          = open_cpu.brl_r_;
    open.brl_g_          = open_cpu.brl_g_;
    open.brl_b_          = open_cpu.brl_b_;
    open.brl_rng_        = open_cpu.brl_rng_;
    open.brl_st_         = open_cpu.brl_st_;
    open.brlp_           = open_cpu.brlp_;
    open.brlp_r_         = open_cpu.brlp_r_;
    open.brlp_g_         = open_cpu.brlp_g_;
    open.brlp_b_         = open_cpu.brlp_b_;
    open.hc_r_           = open_cpu.hc_r_;
    open.hc_r_rng_       = open_cpu.hc_r_rng_;
    open.hs_r_           = open_cpu.hs_r_;
    open.hs_r_rng_       = open_cpu.hs_r_rng_;
    open.hs_g_           = open_cpu.hs_g_;
    open.hs_g_rng_       = open_cpu.hs_g_rng_;
    open.hs_b_           = open_cpu.hs_b_;
    open.hs_b_rng_       = open_cpu.hs_b_rng_;
    open.hs_c_           = open_cpu.hs_c_;
    open.hs_c_rng_       = open_cpu.hs_c_rng_;
    open.hs_m_           = open_cpu.hs_m_;
    open.hs_m_rng_       = open_cpu.hs_m_rng_;
    open.hs_y_           = open_cpu.hs_y_;
    open.hs_y_rng_       = open_cpu.hs_y_rng_;
    open.ts_x1_          = open_cpu.ts_x1_;
    open.ts_y1_          = open_cpu.ts_y1_;
    open.ts_x0_          = open_cpu.ts_x0_;
    open.ts_y0_          = open_cpu.ts_y0_;
    open.ts_s0_          = open_cpu.ts_s0_;
    open.ts_p_           = open_cpu.ts_p_;
    open.ts_s10_         = open_cpu.ts_s10_;
    open.ts_m1_          = open_cpu.ts_m1_;
    open.ts_m2_          = open_cpu.ts_m2_;
    open.ts_s_           = open_cpu.ts_s_;
    open.ts_dsc_         = open_cpu.ts_dsc_;
    open.pt_cmp_Lf_      = open_cpu.pt_cmp_Lf_;
    open.s_Lp100_        = open_cpu.s_Lp100_;
    open.ts_s1_          = open_cpu.ts_s1_;

    return fused;
  }

 private:
  static void HashCombine(std::uint64_t& seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
  }

  static auto FloatBits(float value) -> std::uint64_t {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<std::uint64_t>(bits);
  }

  static void HashFloatArray(std::uint64_t& seed, const float* values, int count) {
    for (int i = 0; i < count; ++i) {
      HashCombine(seed, FloatBits(values[i]));
    }
  }

  static auto BuildHsMaskBaseCacheKey(const OperatorParams& params) -> std::uint64_t {
    std::uint64_t key = 0x48f8d5ed4b4f4b4cULL;
    HashCombine(key, static_cast<std::uint64_t>(params.to_ws_enabled_));
    HashCombine(key, static_cast<std::uint64_t>(params.exposure_enabled_));
    HashCombine(key, FloatBits(params.exposure_offset_));
    HashCombine(key, static_cast<std::uint64_t>(params.contrast_enabled_));
    HashCombine(key, FloatBits(params.contrast_scale_));
    HashCombine(key, static_cast<std::uint64_t>(params.white_enabled_));
    HashCombine(key, static_cast<std::uint64_t>(params.black_enabled_));
    HashCombine(key, FloatBits(params.white_point_));
    HashCombine(key, FloatBits(params.black_point_));
    HashCombine(key, FloatBits(params.slope_));
    HashCombine(key, static_cast<std::uint64_t>(params.color_temp_enabled_));
    HashCombine(key, static_cast<std::uint64_t>(params.color_temp_mode_));
    HashCombine(key, FloatBits(params.color_temp_resolved_cct_));
    HashCombine(key, FloatBits(params.color_temp_resolved_tint_));
    HashFloatArray(key, params.color_temp_resolved_xy_, 2);
    HashCombine(key, static_cast<std::uint64_t>(params.raw_runtime_valid_));
    HashCombine(key, static_cast<std::uint64_t>(params.raw_decode_input_space_));
    HashFloatArray(key, params.raw_cam_mul_, 3);
    HashFloatArray(key, params.raw_pre_mul_, 3);
    HashFloatArray(key, params.raw_cam_xyz_, 9);
    HashCombine(key, static_cast<std::uint64_t>(params.color_temp_matrices_valid_));
    HashFloatArray(key, params.color_temp_cam_to_ap1_, 9);
    HashCombine(key, static_cast<std::uint64_t>(params.hs_local_tone_enabled_));
    HashCombine(key, FloatBits(params.hs_base_radius_));
    HashCombine(key, static_cast<std::uint64_t>(params.hs_base_gaussian_tap_count_));
    HashFloatArray(key, params.hs_base_gaussian_weights_,
                   OperatorParams::kDetailMaxGaussianTapCount);
    HashCombine(key, FloatBits(params.hs_shadow_log_pivot_));
    HashCombine(key, FloatBits(params.hs_shadow_log_width_));
    HashCombine(key, FloatBits(params.hs_highlight_log_pivot_));
    HashCombine(key, FloatBits(params.hs_highlight_log_width_));
    HashCombine(key, params.render_source_cache_key_);
    return key;
  }
};

}  // namespace alcedo
