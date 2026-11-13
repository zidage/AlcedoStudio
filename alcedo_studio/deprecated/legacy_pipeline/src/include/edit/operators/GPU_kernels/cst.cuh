//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>
#include <device_types.h>
#include <texture_fetch_functions.h>
#include <vector_functions.h>

#include "edit/runtime/cuda/drt/disp_enc_funcs.cuh"
#include "edit/runtime/cuda/drt/odt_funcs.cuh"
#include "edit/runtime/cuda/drt/open_drt_funcs.cuh"
#include "edit/runtime/cuda/drt/util_funcs.cuh"
#include "edit/operators/op_kernel.hpp"
#include "param.cuh"

namespace alcedo {
namespace CUDA {

// ============================================================================
// Color Space Transform Constants
// ============================================================================

// ACES2065-1 (AP0) to ACEScg (AP1) transformation matrix
// Reference: ACES Technical Bulletin TB-2014-004
__device__ __constant__ float   AP0_TO_AP1_MAT[9] = {1.4514393161f,  -0.2365107469f, -0.2149285693f,
                                                     -0.0765537734f, 1.1762296998f,  -0.0996759264f,
                                                     0.0083161484f,  -0.0060324498f, 0.9977163014f};

// ACEScg (AP1) to sRGB/Rec.709 transformation matrix
// Reference: ACES Technical Bulletin TB-2014-004
__device__ __constant__ float   AP1_TO_SRGB_MAT[9]   = {1.7050509f,  -0.6217921f, -0.0832588f,
                                                        -0.1302564f, 1.1408052f,  -0.0105488f,
                                                        -0.0240033f, -0.1289690f, 1.1529723f};

// ACEScc encoding constants
__device__ __constant__ float   ACESCC_LOG2_MIN      = -15.0f;             // log2(2^-15)
__device__ __constant__ float   ACESCC_LOG2_DENORM   = -16.0f;             // log2(2^-16)
__device__ __constant__ float   ACESCC_DENORM_TRANS  = 0.00003051757812f;  // 2^-15
__device__ __constant__ float   ACESCC_DENORM_OFFSET = 0.00001525878906f;  // 2^-16
__device__ __constant__ float   ACESCC_A             = 9.72f;
__device__ __constant__ float   ACESCC_B             = 17.52f;

// Reference Gamut Compress
// From
// https://github.com/aces-aswf/aces-core/blob/v1.3/transforms/ctl/lmt/LMT.Academy.GamutCompress.ctl
// Copyright © 2015 Academy of Motion Picture Arts and Sciences (A.M.P.A.S.). Portions contributed
// by others as indicated. All rights reserved.
// Refer to the above URL for full copyright and license information.
__device__ __constant__ float   RGC_LIM_CYAN         = 1.147f;
__device__ __constant__ float   RGC_LIM_MAGENTA      = 1.264f;
__device__ __constant__ float   RGC_LIM_YELLOW       = 1.312f;
__device__ __constant__ float   RGC_THR_CYAN         = 0.815f;
__device__ __constant__ float   RGC_THR_MAGENTA      = 0.803f;
__device__ __constant__ float   RGC_THR_YELLOW       = 0.880f;
__device__ __constant__ float   RGC_PWR              = 1.2f;

// ============================================================================
// Helper Functions
// ============================================================================

// Apply 3x3 matrix transformation: out = M * in
__device__ __forceinline__ void apply_matrix3x3(const float mat[9], float r, float g, float b,
                                                float* out_r, float* out_g, float* out_b) {
  *out_r = mat[0] * r + mat[1] * g + mat[2] * b;
  *out_g = mat[3] * r + mat[4] * g + mat[5] * b;
  *out_b = mat[6] * r + mat[7] * g + mat[8] * b;
}

__device__ __forceinline__ float rgc_compress_curve(float dist, float lim, float thr, float pwr) {
  if (dist < thr) {
    return dist;
  } else {
    // Precomputed parts can be extracted as constants for further optimization, but direct
    // computation on GPU is also fast
    float t_diff      = lim - thr;
    float one_minus_t = 1.0f - thr;

    // Calculate scaling factor scl
    // scl = (lim - thr) / pow(pow((1.0 - thr) / (lim - thr), -pwr) - 1.0, 1.0 / pwr);
    float inner_pow   = powf(one_minus_t / t_diff, -pwr);
    // Prevent division by zero and negative root risks
    float denom       = powf(fmaxf(0.0f, inner_pow - 1.0f), 1.0f / pwr);
    float scl         = (denom > 1e-6f) ? (t_diff / denom) : 0.0f;

    float nd          = (dist - thr) / scl;
    float p           = powf(fmaxf(0.0f, nd), pwr);

    // Compression calculation
    return thr + scl * nd / (powf(1.0f + p, 1.0f / pwr));
  }
}

// ACEScc log encoding: linear (AP1) -> ACEScc
// Reference: S-2014-003 ACEScc – A Quasi-Logarithmic Encoding of ACES Data
__device__ __forceinline__ float acescc_encode(float x) {
  const float encode_floor = (ACESCC_LOG2_DENORM + ACESCC_A) / ACESCC_B;

  if (x <= 0.0f) {
    // Appendix A leaves negative-value preservation implementation-defined.
    // Keep the official ACEScc range for non-negative AP1 values and place
    // preserved negatives below the standard ACEScc floor so round-trips stay lossless.
    return encode_floor + x;
  }
  if (x < ACESCC_DENORM_TRANS) {
    // Denormalized region: linear ramp to avoid log(0)
    return (log2f(ACESCC_DENORM_OFFSET + x * 0.5f) + ACESCC_A) / ACESCC_B;
  }
  // Normal region: standard log encoding
  return (log2f(x) + ACESCC_A) / ACESCC_B;
}
// ACEScc log decoding: ACEScc -> linear (AP1)
// Reference: S-2014-003 ACEScc – A Quasi-Logarithmic Encoding of ACES Data
__device__ __forceinline__ float acescc_decode(float acescc) {
  const float encode_floor     = (ACESCC_LOG2_DENORM + ACESCC_A) / ACESCC_B;
  // Threshold for denormalized region: (log2(2^-15) + 9.72) / 17.52
  const float denorm_threshold = (ACESCC_LOG2_MIN + ACESCC_A) / ACESCC_B;  // ≈ -0.3013698630

  if (acescc < encode_floor) {
    return acescc - encode_floor;
  }
  if (acescc <= denorm_threshold) {
    // Denormalized region
    return (exp2f(acescc * ACESCC_B - ACESCC_A) - ACESCC_DENORM_OFFSET) * 2.0f;
  }
  // Normal region
  return exp2f(acescc * ACESCC_B - ACESCC_A);
}
// Gamma 2.2 encoding: linear -> gamma encoded
__device__ __forceinline__ float gamma22_encode(float linear) {
  // Clamp to avoid NaN from negative values
  linear = fmaxf(linear, 0.0f);
  return powf(linear, 1.0f / 2.2f);
}

// ============================================================================
// Color Space Transform Kernels
// ============================================================================

/**
 * GPU_TOWS_Kernel: ACES2065-1 (AP0, linear) -> AP1 (RGCed) -> ACEScc (AP1, log encoded)
 *
 * Pipeline:
 *   1. Transform from ACES2065-1 (AP0) primaries to ACEScg (AP1) primaries
 *   2. Apply Reference Gamut Compression (RGC)
 *   3. Apply ACEScc logarithmic encoding curve
 */
struct GPU_TOWS_Kernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.to_ws_enabled_) return;

    float      ap1_r, ap1_g, ap1_b;
    const bool use_camera_to_ap1 =
        (params.raw_decode_input_space_ == 1) && params.color_temp_matrices_valid_;
    if (use_camera_to_ap1) {
      apply_matrix3x3(params.color_temp_cam_to_ap1_, p->x, p->y, p->z, &ap1_r, &ap1_g, &ap1_b);
    } else {
      // Legacy path assumes decode input is AP0.
      apply_matrix3x3(AP0_TO_AP1_MAT, p->x, p->y, p->z, &ap1_r, &ap1_g, &ap1_b);
    }

    // // Step 2: Apply RGC
    float ach     = fmaxf(ap1_r, fmaxf(ap1_g, ap1_b));

    float abs_ach = fabsf(ach);
    if (abs_ach > 1e-6f) {
      float dist_cyan    = (ach - ap1_r) / abs_ach;
      float dist_magenta = (ach - ap1_g) / abs_ach;
      float dist_yellow  = (ach - ap1_b) / abs_ach;

      // Apply RGC compression curve
      float c_dist_cyan  = rgc_compress_curve(dist_cyan, RGC_LIM_CYAN, RGC_THR_CYAN, RGC_PWR);
      float c_dist_magenta =
          rgc_compress_curve(dist_magenta, RGC_LIM_MAGENTA, RGC_THR_MAGENTA, RGC_PWR);
      float c_dist_yellow =
          rgc_compress_curve(dist_yellow, RGC_LIM_YELLOW, RGC_THR_YELLOW, RGC_PWR);

      // Reconstruct compressed AP1 values
      ap1_r = ach - c_dist_cyan * abs_ach;
      ap1_g = ach - c_dist_magenta * abs_ach;
      ap1_b = ach - c_dist_yellow * abs_ach;
    }

    // Step 3: Apply ACEScc log encoding
    float acescc_r = acescc_encode(ap1_r);
    float acescc_g = acescc_encode(ap1_g);
    float acescc_b = acescc_encode(ap1_b);

    *p             = make_float4(acescc_r, acescc_g, acescc_b, p->w);
  }
};

struct GPU_LMT_Kernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.lmt_enabled_) return;
    if (params.lmt_lut_.texture_object_ == 0 || params.lmt_lut_.edge_size_ <= 1) return;

    float scale =
        (params.lmt_lut_.edge_size_ - 1.0f) / static_cast<float>(params.lmt_lut_.edge_size_);
    float  offset = 1.0f / (2.0f * params.lmt_lut_.edge_size_);
    float  u      = (p->x * scale + offset);
    float  v      = (p->y * scale + offset);
    float  w      = (p->z * scale + offset);

    float4 result = tex3D<float4>(params.lmt_lut_.texture_object_, u, v, w);
    *p            = make_float4(result.x, result.y, result.z, p->w);
  }
};

/**
 * GPU_OUTPUT_Kernel: RRT + ODT
 *
 * Pipeline: Refer to ACES Output Transform documentation
 *
 */
struct GPU_OUTPUT_Kernel : GPUPointOpTag {
  __device__ __forceinline__ void operator()(float4* p, GPUOperatorParams& params) const {
    if (!params.to_output_enabled_) return;

    const float3 aces_linear =
        make_float3(acescc_decode(p->x), acescc_decode(p->y), acescc_decode(p->z));

    float3 odt_color;
    if (params.to_output_params_.method_ == CudaDrtMethod::ACES_2_0) {
      odt_color = OutputTransform_fwd(aces_linear, params.to_output_params_.aces_params_);
    } else {
      odt_color = OpenDRTTransform_fwd(aces_linear, params.to_output_params_.open_drt_params_);
    }

    const float3 cv_3 = DisplayEncoding(odt_color, params.to_output_params_.limit_to_display_matx,
                                        params.to_output_params_.eotf,
                                        params.to_output_params_.display_linear_scale_);

    *p                = make_float4(cv_3.x, cv_3.y, cv_3.z, p->w);
  }
};
};  // namespace CUDA
};  // namespace alcedo
