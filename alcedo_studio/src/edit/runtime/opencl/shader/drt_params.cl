//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifndef ALCEDO_OPENCL_DRT_PARAMS_CL
#define ALCEDO_OPENCL_DRT_PARAMS_CL

#define ALCEDO_OPENCL_ACES_ODT_TABLE_SIZE 362
#define ALCEDO_OPENCL_SHARED_TONE_CURVE_CONTROL_POINT_COUNT 5
#define ALCEDO_OPENCL_HLS_PROFILE_COUNT 8
#define ALCEDO_OPENCL_MAX_CURVE_CONTROL_POINTS 32
#define ALCEDO_OPENCL_DETAIL_MAX_GAUSSIAN_TAP_COUNT 64

typedef struct {
  float MATRIX_RGB_to_CAM16_c_[9];
  float MATRIX_CAM16_c_to_RGB_[9];
  float MATRIX_cone_response_to_Aab_[9];
  float MATRIX_Aab_to_cone_response_[9];
  float F_L_n_;
  float cz_;
  float inv_cz_;
  float A_w_J_;
  float inv_A_w_J_;
} OpenClJMhParams;

typedef struct {
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
} OpenClTSParams;

typedef struct {
  float            peak_luminance_;
  OpenClJMhParams  input_params_;
  OpenClJMhParams  reach_params_;
  OpenClJMhParams  limit_params_;
  OpenClTSParams   ts_;
  float            limit_J_max;
  float            model_gamma_inv;
  float            mid_J;
  float            focus_dist;
  float            lower_hull_gamma_inv;
  int              hue_linearity_search_range[2];
  float            sat;
  float            sat_thr;
  float            compr;
  float            chroma_compress_scale;
  float            table_reach_M_[ALCEDO_OPENCL_ACES_ODT_TABLE_SIZE];
  float            table_hues_[ALCEDO_OPENCL_ACES_ODT_TABLE_SIZE];
  float            table_upper_hull_gamma_[ALCEDO_OPENCL_ACES_ODT_TABLE_SIZE];
  float            table_gamut_cusps_[ALCEDO_OPENCL_ACES_ODT_TABLE_SIZE][4];
} OpenClODTParams;

typedef struct {
  int   tn_hcon_enable_;
  int   tn_lcon_enable_;
  int   pt_enable_;
  int   ptl_enable_;
  int   ptm_enable_;
  int   brl_enable_;
  int   brlp_enable_;
  int   hc_enable_;
  int   hs_rgb_enable_;
  int   hs_cmy_enable_;
  int   creative_white_;
  int   surround_;
  int   clamp_;
  int   display_gamut_;
  int   display_eotf_;
  float tn_con_;
  float tn_sh_;
  float tn_toe_;
  float tn_off_;
  float tn_hcon_;
  float tn_hcon_pv_;
  float tn_hcon_st_;
  float tn_lcon_;
  float tn_lcon_w_;
  float cwp_lm_;
  float rs_sa_;
  float rs_rw_;
  float rs_bw_;
  float pt_lml_;
  float pt_lml_r_;
  float pt_lml_g_;
  float pt_lml_b_;
  float pt_lmh_;
  float pt_lmh_r_;
  float pt_lmh_b_;
  float ptl_c_;
  float ptl_m_;
  float ptl_y_;
  float ptm_low_;
  float ptm_low_rng_;
  float ptm_low_st_;
  float ptm_high_;
  float ptm_high_rng_;
  float ptm_high_st_;
  float brl_;
  float brl_r_;
  float brl_g_;
  float brl_b_;
  float brl_rng_;
  float brl_st_;
  float brlp_;
  float brlp_r_;
  float brlp_g_;
  float brlp_b_;
  float hc_r_;
  float hc_r_rng_;
  float hs_r_;
  float hs_r_rng_;
  float hs_g_;
  float hs_g_rng_;
  float hs_b_;
  float hs_b_rng_;
  float hs_c_;
  float hs_c_rng_;
  float hs_m_;
  float hs_m_rng_;
  float hs_y_;
  float hs_y_rng_;
  float ts_x1_;
  float ts_y1_;
  float ts_x0_;
  float ts_y0_;
  float ts_s0_;
  float ts_p_;
  float ts_s10_;
  float ts_m1_;
  float ts_m2_;
  float ts_s_;
  float ts_dsc_;
  float pt_cmp_Lf_;
  float s_Lp100_;
  float ts_s1_;
} OpenClOpenDRTParams;

typedef struct {
  int                 method_;
  int                 eotf_;
  OpenClODTParams     aces_params_;
  OpenClOpenDRTParams open_drt_params_;
  float               limit_to_display_matx[9];
  float               display_linear_scale_;
} OpenClToOutputParams;

typedef struct {
  uint  exposure_enabled_;
  float exposure_offset_;
  uint  contrast_enabled_;
  float contrast_scale_;
  uint  shadows_enabled_;
  float shadows_offset_;
  float shadows_x0_;
  float shadows_x1_;
  float shadows_y0_;
  float shadows_y1_;
  float shadows_m0_;
  float shadows_m1_;
  float shadows_dx_;
  uint  highlights_enabled_;
  float highlights_k_;
  float highlights_offset_;
  float highlights_slope_range_;
  float highlights_m0_;
  float highlights_m1_;
  float highlights_x0_;
  float highlights_y0_;
  float highlights_y1_;
  float highlights_dx_;
  uint  shared_tone_curve_enabled_;
  uint  shared_tone_curve_apply_in_shadows_;
  uint  shared_tone_curve_apply_in_highlights_;
  int   shared_tone_curve_ctrl_pts_size_;
  float shared_tone_curve_ctrl_pts_x_[ALCEDO_OPENCL_SHARED_TONE_CURVE_CONTROL_POINT_COUNT];
  float shared_tone_curve_ctrl_pts_y_[ALCEDO_OPENCL_SHARED_TONE_CURVE_CONTROL_POINT_COUNT];
  float shared_tone_curve_h_[ALCEDO_OPENCL_SHARED_TONE_CURVE_CONTROL_POINT_COUNT - 1];
  float shared_tone_curve_m_[ALCEDO_OPENCL_SHARED_TONE_CURVE_CONTROL_POINT_COUNT];
  uint  hs_local_tone_enabled_;
  float hs_base_radius_;
  int   hs_base_gaussian_tap_count_;
  float hs_base_gaussian_weights_[ALCEDO_OPENCL_DETAIL_MAX_GAUSSIAN_TAP_COUNT];
  float hs_shadow_log_pivot_;
  float hs_shadow_log_width_;
  float hs_highlight_log_pivot_;
  float hs_highlight_log_width_;
  uint  render_roi_enabled_;
  int   render_roi_x_;
  int   render_roi_y_;
  float render_roi_scale_x_;
  float render_roi_scale_y_;
  int   render_roi_reference_width_;
  int   render_roi_reference_height_;
  uint  white_enabled_;
  float white_point_;
  uint  black_enabled_;
  float black_point_;
  float slope_;
  uint  hls_enabled_;
  float target_hls_[3];
  float hls_adjustment_[3];
  float hue_range_;
  float lightness_range_;
  float saturation_range_;
  int   hls_profile_count_;
  float hls_profile_hues_[ALCEDO_OPENCL_HLS_PROFILE_COUNT];
  float hls_profile_adjustments_[ALCEDO_OPENCL_HLS_PROFILE_COUNT][3];
  float hls_profile_hue_ranges_[ALCEDO_OPENCL_HLS_PROFILE_COUNT];
  uint  saturation_enabled_;
  float saturation_offset_;
  uint  tint_enabled_;
  float tint_offset_;
  uint  vibrance_enabled_;
  float vibrance_offset_;
  uint  to_ws_enabled_;
  uint  color_temp_enabled_;
  int   color_temp_mode_;
  float color_temp_resolved_xy_[2];
  uint  raw_runtime_valid_;
  int   raw_decode_input_space_;
  float color_temp_cam_to_ap1_[9];
  uint  color_temp_matrices_valid_;
  uint  lmt_enabled_;
  uint  lmt_lut_enabled_;
  uint  lmt_lut_edge_size_;
  uint  to_output_enabled_;
  OpenClToOutputParams to_output_params_;
  uint  curve_enabled_;
  int   curve_ctrl_pts_size_;
  float curve_ctrl_pts_x_[ALCEDO_OPENCL_MAX_CURVE_CONTROL_POINTS];
  float curve_ctrl_pts_y_[ALCEDO_OPENCL_MAX_CURVE_CONTROL_POINTS];
  float curve_h_[ALCEDO_OPENCL_MAX_CURVE_CONTROL_POINTS - 1];
  float curve_m_[ALCEDO_OPENCL_MAX_CURVE_CONTROL_POINTS];
  uint  clarity_enabled_;
  float clarity_offset_;
  float clarity_radius_;
  uint  sharpen_enabled_;
  float sharpen_offset_;
  float sharpen_radius_;
  float sharpen_threshold_;
  uint  color_wheel_enabled_;
  float lift_color_offset_[3];
  float lift_luminance_offset_;
  float gamma_color_offset_[3];
  float gamma_luminance_offset_;
  float gain_color_offset_[3];
  float gain_luminance_offset_;
} OpenClFusedParams;

#endif
