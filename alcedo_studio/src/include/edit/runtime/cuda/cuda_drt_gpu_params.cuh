//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>
#include <vector_types.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifndef GPU_FUNC
#define GPU_FUNC __device__ __forceinline__
#endif

namespace alcedo {

/** @brief DRT method as the CUDA kernel reads it. Values equal `ColorUtils::ODTMethod`. */
enum class CudaDrtMethod : int {
  ACES_2_0 = 0,
  OPEN_DRT = 1,
};

/** @brief Display encoding EOTF as the CUDA kernel reads it. Values equal `ColorUtils::EOTF`. */
enum class CudaDrtEotf : int {
  LINEAR    = 0,
  ST2084    = 1,
  HLG       = 2,
  GAMMA_2_6 = 3,
  BT1886    = 4,
  GAMMA_2_2 = 5,
  GAMMA_1_8 = 6,
};

struct CudaDrtJmhParams {
  float MATRIX_RGB_to_CAM16_c_[9];
  float MATRIX_CAM16_c_to_RGB_[9];
  float MATRIX_cone_response_to_Aab_[9];
  float MATRIX_Aab_to_cone_response_[9];
  float F_L_n_;  // F_L normalized
  float cz_;
  float inv_cz_;  // 1/cz
  float A_w_J_;
  float inv_A_w_J_;  // 1/A_w_J
};

struct CudaDrtTonescaleParams {
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

/**
 * @brief Device lookup table bound to linear memory through a texture object.
 *
 * The owner of the enclosing parameter struct calls `Reset` to destroy the texture object and
 * free the memory. Copies share the handles; only one copy may call `Reset`.
 */
template <typename T>
struct CudaDrtTable1D {
  cudaTextureObject_t texture_object_ = 0;
  void*               dev_ptr_        = nullptr;
  size_t              count_          = 0;

  CudaDrtTable1D()                    = default;

  void Reset() {
    if (texture_object_) {
      cudaDestroyTextureObject(texture_object_);
      texture_object_ = 0;
    }
    if (dev_ptr_) {
      cudaFree(dev_ptr_);
      dev_ptr_ = nullptr;
    }
    count_ = 0;
  }
};

/**
 * @brief Upload @p count values to device memory and bind a point-sampled texture object.
 *
 * Host-side helper. The CUDA kernels index the table explicitly, so no hardware filtering is
 * used. Returns an empty table when @p host_data is null or @p count is zero.
 */
template <typename T>
auto CreateCudaDrtTable(const T* host_data, size_t count) -> CudaDrtTable1D<T> {
  CudaDrtTable1D<T> table;
  if (!host_data || count == 0) {
    return table;
  }

  table.count_       = count;

  const size_t bytes = sizeof(T) * count;
  cudaMalloc(&table.dev_ptr_, bytes);
  cudaMemcpy(table.dev_ptr_, host_data, bytes, cudaMemcpyHostToDevice);

  cudaResourceDesc res_desc       = {};
  res_desc.resType                = cudaResourceTypeLinear;
  res_desc.res.linear.devPtr      = table.dev_ptr_;
  res_desc.res.linear.desc        = cudaCreateChannelDesc<T>();
  res_desc.res.linear.sizeInBytes = bytes;

  cudaTextureDesc tex_desc        = {};
  tex_desc.normalizedCoords       = 0;
  tex_desc.filterMode             = cudaFilterModePoint;
  tex_desc.readMode               = cudaReadModeElementType;
  tex_desc.addressMode[0]         = cudaAddressModeClamp;

  cudaCreateTextureObject(&table.texture_object_, &res_desc, &tex_desc, nullptr);
  return table;
}

struct CudaDrtAcesParams {
  float                  peak_luminance_ = 100.0f;

  // JMh parameters
  CudaDrtJmhParams       input_params_;
  CudaDrtJmhParams       reach_params_;
  CudaDrtJmhParams       limit_params_;

  // Tonescale parameters
  CudaDrtTonescaleParams ts_;

  // Shared compression parameters
  float                  limit_J_max;
  float                  model_gamma_inv;
  CudaDrtTable1D<float>  table_reach_M_;
  std::uintptr_t         host_table_reach_M_id_ = 0;

  // Chroma compression parameters
  float                  sat;
  float                  sat_thr;
  float                  compr;
  float                  chroma_compress_scale;

  // Gamut compression parameters
  float                  mid_J;
  float                  focus_dist;
  float                  lower_hull_gamma_inv;
  CudaDrtTable1D<float>  table_hues_;
  std::uintptr_t         host_table_hues_id_ = 0;

  // Packed as float4{J, M, h}
  CudaDrtTable1D<float4> table_gamut_cusps_;
  std::uintptr_t         host_table_gamut_cusps_id_ = 0;

  CudaDrtTable1D<float>  table_upper_hull_gamma_;
  std::uintptr_t         host_table_upper_hull_gamma_id_ = 0;

  int                    hue_linearity_search_range[2]   = {0, 1};

  /** @brief Release the four device tables. Scalar fields keep their values. */
  void                   Reset() {
    table_reach_M_.Reset();
    table_hues_.Reset();
    table_gamut_cusps_.Reset();
    table_upper_hull_gamma_.Reset();
    host_table_reach_M_id_          = 0;
    host_table_hues_id_             = 0;
    host_table_gamut_cusps_id_      = 0;
    host_table_upper_hull_gamma_id_ = 0;
    hue_linearity_search_range[0]   = 0;
    hue_linearity_search_range[1]   = 1;
  }
};

struct CudaDrtOpenDrtParams {
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

/**
 * @brief DRT parameter block that the CUDA DRT kernel reads from the parameter arena.
 *
 * The layout is identical to the pre-G10.5 `GPU_TO_OUTPUT_Params`; the assertions below pin it.
 */
struct CudaDrtGpuParams {
  CudaDrtMethod        method_          = CudaDrtMethod::OPEN_DRT;
  CudaDrtAcesParams    aces_params_     = {};
  CudaDrtOpenDrtParams open_drt_params_ = {};
  float                limit_to_display_matx[9];
  float                display_linear_scale_ = 1.0f;
  CudaDrtEotf          eotf                  = CudaDrtEotf::LINEAR;

  /** @brief Release the ACES tables and reset the display fields. */
  void                 Reset() {
    aces_params_.Reset();
    open_drt_params_      = {};
    display_linear_scale_ = 1.0f;
    eotf                  = CudaDrtEotf::LINEAR;
  }
};

// Sizes and offsets of the pre-G10.5 GPU_TO_OUTPUT_Params (MSVC x64, CUDA 12.8), recorded at
// commit 0cf45f45. The DRT kernel reads this block byte for byte from the parameter arena.
static_assert(std::is_standard_layout_v<CudaDrtGpuParams>);
static_assert(sizeof(CudaDrtJmhParams) == 164);
static_assert(sizeof(CudaDrtTonescaleParams) == 44);
static_assert(sizeof(CudaDrtTable1D<float>) == 24);
static_assert(sizeof(CudaDrtTable1D<float4>) == 24);
static_assert(offsetof(CudaDrtTable1D<float>, texture_object_) == 0);
static_assert(offsetof(CudaDrtTable1D<float>, dev_ptr_) == 8);
static_assert(offsetof(CudaDrtTable1D<float>, count_) == 16);
static_assert(sizeof(CudaDrtAcesParams) == 720);
static_assert(offsetof(CudaDrtAcesParams, peak_luminance_) == 0);
static_assert(offsetof(CudaDrtAcesParams, input_params_) == 4);
static_assert(offsetof(CudaDrtAcesParams, reach_params_) == 168);
static_assert(offsetof(CudaDrtAcesParams, limit_params_) == 332);
static_assert(offsetof(CudaDrtAcesParams, ts_) == 496);
static_assert(offsetof(CudaDrtAcesParams, limit_J_max) == 540);
static_assert(offsetof(CudaDrtAcesParams, model_gamma_inv) == 544);
static_assert(offsetof(CudaDrtAcesParams, table_reach_M_) == 552);
static_assert(offsetof(CudaDrtAcesParams, host_table_reach_M_id_) == 576);
static_assert(offsetof(CudaDrtAcesParams, sat) == 584);
static_assert(offsetof(CudaDrtAcesParams, sat_thr) == 588);
static_assert(offsetof(CudaDrtAcesParams, compr) == 592);
static_assert(offsetof(CudaDrtAcesParams, chroma_compress_scale) == 596);
static_assert(offsetof(CudaDrtAcesParams, mid_J) == 600);
static_assert(offsetof(CudaDrtAcesParams, focus_dist) == 604);
static_assert(offsetof(CudaDrtAcesParams, lower_hull_gamma_inv) == 608);
static_assert(offsetof(CudaDrtAcesParams, table_hues_) == 616);
static_assert(offsetof(CudaDrtAcesParams, host_table_hues_id_) == 640);
static_assert(offsetof(CudaDrtAcesParams, table_gamut_cusps_) == 648);
static_assert(offsetof(CudaDrtAcesParams, host_table_gamut_cusps_id_) == 672);
static_assert(offsetof(CudaDrtAcesParams, table_upper_hull_gamma_) == 680);
static_assert(offsetof(CudaDrtAcesParams, host_table_upper_hull_gamma_id_) == 704);
static_assert(offsetof(CudaDrtAcesParams, hue_linearity_search_range) == 712);
static_assert(sizeof(CudaDrtOpenDrtParams) == 328);
static_assert(offsetof(CudaDrtOpenDrtParams, tn_con_) == 60);
static_assert(offsetof(CudaDrtOpenDrtParams, hs_y_rng_) == 268);
static_assert(offsetof(CudaDrtOpenDrtParams, ts_x1_) == 272);
static_assert(offsetof(CudaDrtOpenDrtParams, ts_s1_) == 324);
static_assert(sizeof(CudaDrtGpuParams) == 1104);
static_assert(offsetof(CudaDrtGpuParams, method_) == 0);
static_assert(offsetof(CudaDrtGpuParams, aces_params_) == 8);
static_assert(offsetof(CudaDrtGpuParams, open_drt_params_) == 728);
static_assert(offsetof(CudaDrtGpuParams, limit_to_display_matx) == 1056);
static_assert(offsetof(CudaDrtGpuParams, display_linear_scale_) == 1092);
static_assert(offsetof(CudaDrtGpuParams, eotf) == 1096);

}  // namespace alcedo
