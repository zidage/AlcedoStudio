//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of helper functions in Lib.Academy.DisplayEncoding.ctl
// Reference:
// https://github.com/aces-aswf/aces-core/blob/main/lib/Lib.Academy.DisplayEncoding.ctl

#pragma once

#include <cuda_runtime.h>
#include <vector_functions.h>
#include <vector_types.h>

#include "disp_enc_const.cuh"
#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"
#include "odt_const.cuh"
#include "util_funcs.cuh"

namespace alcedo {
namespace CUDA {


GPU_FUNC float moncurve_fwd(float x, float gamma, float offs) {
  const float fs =
      ((gamma - 1.0f) / offs) * powf(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float xb = offs / (gamma - 1.0f);
  return (x >= xb) ? powf((x + offs) / (1.0f + offs), gamma) : x * fs;
}

GPU_FUNC float moncurve_inv(float y, float gamma, float offs) {
  const float yb = powf(offs * gamma / ((gamma - 1.0f) * (1.0f + offs)), gamma);
  const float rs = powf((gamma - 1.0f) / offs, gamma - 1.0f) * powf((1.0f + offs) / gamma, gamma);
  return (y >= yb) ? (1.0f + offs) * powf(y, 1.0f / gamma) - offs : y * rs;
}

GPU_FUNC float3 moncurve_fwd_f3(const float3& v, float gamma, float offs) {
  return make_float3(moncurve_fwd(v.x, gamma, offs), moncurve_fwd(v.y, gamma, offs),
                     moncurve_fwd(v.z, gamma, offs));
}

GPU_FUNC float3 moncurve_inv_f3(const float3& v, float gamma, float offs) {
  return make_float3(moncurve_inv(v.x, gamma, offs), moncurve_inv(v.y, gamma, offs),
                     moncurve_inv(v.z, gamma, offs));
}

// The forward OTF specified in Rec. ITU-R BT.1886
// L = a(max[(V+b),0])^g
GPU_FUNC float bt1886_fwd(float V, float gamma, float Lw = 1.0f, float Lb = 0.0f) {
  float a = powf(powf(Lw, 1.f / gamma) - powf(Lb, 1.f / gamma), gamma);
  float b = powf(Lb, 1.f / gamma) / (powf(Lw, 1.f / gamma) - powf(Lb, 1.f / gamma));
  float L = a * powf(max(V + b, 0.f), gamma);
  return L;
}

// The reverse EOTF specified in Rec. ITU-R BT.1886
// L = a(max[(V+b),0])^g
GPU_FUNC float bt1886_inv(float L, float gamma, float Lw = 1.0f, float Lb = 0.0f) {
  float a = powf(powf(Lw, 1.f / gamma) - powf(Lb, 1.f / gamma), gamma);
  float b = powf(Lb, 1.f / gamma) / (powf(Lw, 1.f / gamma) - powf(Lb, 1.f / gamma));
  float V = powf(max(L / a, 0.f), 1.f / gamma) - b;
  return V;
}

GPU_FUNC float3 bt1886_fwd_f3(const float3& v, float gamma, float Lw = 1.0f, float Lb = 0.0f) {
  return make_float3(bt1886_fwd(v.x, gamma, Lw, Lb), bt1886_fwd(v.y, gamma, Lw, Lb),
                     bt1886_fwd(v.z, gamma, Lw, Lb));
}

GPU_FUNC float3 bt1886_inv_f3(const float3& v, float gamma, float Lw = 1.0f, float Lb = 0.0f) {
  return make_float3(bt1886_inv(v.x, gamma, Lw, Lb), bt1886_inv(v.y, gamma, Lw, Lb),
                     bt1886_inv(v.z, gamma, Lw, Lb));
}

GPU_FUNC float SMPTE_range_to_full(float in) {
  const float REF_BLACK = (64.f / 1023.f);
  const float REF_WHITE = (940.f / 1023.f);
  return (in - REF_BLACK) / (REF_WHITE - REF_BLACK);
}

GPU_FUNC float full_to_SMPTE_range(float in) {
  const float REF_BLACK = (64.f / 1023.f);
  const float REF_WHITE = (940.f / 1023.f);
  return in * (REF_WHITE - REF_BLACK) + REF_BLACK;
}

GPU_FUNC float3 SMPTE_range_to_full_f3(const float3& v) {
  return make_float3(SMPTE_range_to_full(v.x), SMPTE_range_to_full(v.y), SMPTE_range_to_full(v.z));
}

GPU_FUNC float3 full_to_SMPTE_range_f3(const float3& v) {
  return make_float3(full_to_SMPTE_range(v.x), full_to_SMPTE_range(v.y), full_to_SMPTE_range(v.z));
}

// Base functions from SMPTE ST 2084-2014

// Converts from the non-linear perceptually quantized space to linear cd/m^2
// Note that this is in float, and assumes normalization from 0 - 1
// (0 - pq_C for linear) and does not handle the integer coding in the Annex
// sections of SMPTE ST 2084-2014
GPU_FUNC float ST2084_to_Y(float N) {
  // Note that this does NOT handle any of the signal range
  // considerations from 2084 - this assumes full range (0 - 1)
  float Np = powf(N, 1.0f / pq_m2);
  float L  = Np - pq_c1;
  if (L < 0.0f) L = 0.0f;
  L = L / (pq_c2 - pq_c3 * Np);
  L = powf(L, 1.0f / pq_m1);
  return L * pq_C;  // returns cd/m^2
}

// Converts from linear cd/m^2 to the non-linear perceptually quantized space
// Note that this is in float, and assumes normalization from 0 - 1
// (0 - pq_C for linear) and does not handle the integer coding in the Annex
// sections of SMPTE ST 2084-2014
GPU_FUNC float Y_to_ST2084(float C) {
  // Note that this does NOT handle any of the signal range
  // considerations from 2084 - this returns full range (0 - 1)
  float L  = C / pq_C;
  float Lm = pow(L, pq_m1);
  float N  = (pq_c1 + pq_c2 * Lm) / (1.0 + pq_c3 * Lm);
  N        = pow(N, pq_m2);
  return N;
}

// Converts from linear cd/m^2 to PQ code values
GPU_FUNC float3 Y_to_ST2084_f3(const float3& C) {
  return make_float3(Y_to_ST2084(C.x), Y_to_ST2084(C.y), Y_to_ST2084(C.z));
}

// Converts from PQ code values to linear cd/m^2
GPU_FUNC float3 ST2084_to_Y_f3(const float3& N) {
  return make_float3(ST2084_to_Y(N.x), ST2084_to_Y(N.y), ST2084_to_Y(N.z));
}

// Conversion of PQ signal to HLG, as detailed in Section 7 of ITU-R BT.2390-0
GPU_FUNC float3 ST2084_to_HLG_1000nits_f3(const float3& PQ) {
  // ST.2084 EOTF (PQ -> display linear)
  float3 display_linear = ST2084_to_Y_f3(PQ);

  // HLG Inverse OOTF (display linear -> scene linear)
  float  Y_d = 0.2627f * display_linear.x + 0.6780f * display_linear.y + 0.0593f * display_linear.z;
  const float L_w   = 1000.f;
  const float L_b   = 0.f;
  const float alpha = (L_w - L_b);
  const float beta  = L_b;
  const float gamma = 1.2f;

  float3      scene_linear;
  if (Y_d == 0.f) {
    scene_linear = make_float3(0.f, 0.f, 0.f);
  } else {
    float p        = powf((Y_d - beta) / alpha, (1.f - gamma) / gamma);
    scene_linear.x = p * ((display_linear.x - beta) / alpha);
    scene_linear.y = p * ((display_linear.y - beta) / alpha);
    scene_linear.z = p * ((display_linear.z - beta) / alpha);
  }

  // HLG OETF (scene linear -> non-linear signal)
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;

  float3      HLG;
  HLG.x = (scene_linear.x <= (1.f / 12.f)) ? sqrtf(3.f * scene_linear.x)
                                           : a * logf(12.f * scene_linear.x - b) + c;
  HLG.y = (scene_linear.y <= (1.f / 12.f)) ? sqrtf(3.f * scene_linear.y)
                                           : a * logf(12.f * scene_linear.y - b) + c;
  HLG.z = (scene_linear.z <= (1.f / 12.f)) ? sqrtf(3.f * scene_linear.z)
                                           : a * logf(12.f * scene_linear.z - b) + c;

  return HLG;
}

GPU_FUNC float3 HLG_1000nits_to_ST2084_f3(const float3& HLG) {
  // HLG EOTF (non-linear signal -> scene linear)
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;

  float3      scene_linear;
  scene_linear.x    = (HLG.x <= 0.5f) ? (HLG.x * HLG.x) / 3.f : (expf((HLG.x - c) / a) + b) / 12.f;
  scene_linear.y    = (HLG.y <= 0.5f) ? (HLG.y * HLG.y) / 3.f : (expf((HLG.y - c) / a) + b) / 12.f;
  scene_linear.z    = (HLG.z <= 0.5f) ? (HLG.z * HLG.z) / 3.f : (expf((HLG.z - c) / a) + b) / 12.f;

  // HLG OOTF (scene linear -> display linear)
  const float L_w   = 1000.f;
  const float L_b   = 0.f;
  const float alpha = (L_w - L_b);
  const float beta  = L_b;
  const float gamma = 1.2f;
  float3      display_linear;

  float       Y_s = 0.2627f * scene_linear.x + 0.6780f * scene_linear.y + 0.0593f * scene_linear.z;
  if (Y_s == 0.f) {
    display_linear = make_float3(0.f, 0.f, 0.f);
  } else {
    float p          = powf(Y_s, gamma / (1.f - gamma));
    display_linear.x = alpha * scene_linear.x / p + beta;
    display_linear.y = alpha * scene_linear.y / p + beta;
    display_linear.z = alpha * scene_linear.z / p + beta;
  }

  // ST.2084 OETF (display linear -> PQ)
  float3 PQ = Y_to_ST2084_f3(display_linear);
  return PQ;
}

GPU_FUNC float3 HLG_from_display_linear_1000nits_f3(const float3& display_linear) {
  float Yd = 0.2627f * display_linear.x + 0.6780f * display_linear.y + 0.0593f * display_linear.z;
  float3 rgb = display_linear;
  if (Yd > 0.0f) {
    rgb = mult_f_f3(rgb, powf(Yd, (1.0f - 1.2f) / 1.2f));
  }

  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;
  rgb.x         = (rgb.x <= (1.0f / 12.0f)) ? sqrtf(3.0f * rgb.x) : a * logf(12.0f * rgb.x - b) + c;
  rgb.y         = (rgb.y <= (1.0f / 12.0f)) ? sqrtf(3.0f * rgb.y) : a * logf(12.0f * rgb.y - b) + c;
  rgb.z         = (rgb.z <= (1.0f / 12.0f)) ? sqrtf(3.0f * rgb.z) : a * logf(12.0f * rgb.z - b) + c;
  return rgb;
}

GPU_FUNC float3 HLG_to_display_linear_1000nits_f3(const float3& hlg_signal) {
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;

  float3 rgb;
  rgb.x = (hlg_signal.x <= 0.5f) ? (hlg_signal.x * hlg_signal.x) / 3.0f
                                 : (expf((hlg_signal.x - c) / a) + b) / 12.0f;
  rgb.y = (hlg_signal.y <= 0.5f) ? (hlg_signal.y * hlg_signal.y) / 3.0f
                                 : (expf((hlg_signal.y - c) / a) + b) / 12.0f;
  rgb.z = (hlg_signal.z <= 0.5f) ? (hlg_signal.z * hlg_signal.z) / 3.0f
                                 : (expf((hlg_signal.z - c) / a) + b) / 12.0f;

  const float Ys = 0.2627f * rgb.x + 0.6780f * rgb.y + 0.0593f * rgb.z;
  if (Ys > 0.0f) {
    rgb = mult_f_f3(rgb, powf(Ys, 1.2f - 1.0f));
  }
  return rgb;
}

GPU_FUNC float3 eotf_inv(const float3& rgb_linear_in, CudaDrtEotf otf_type) {
  float3 rgb_linear = make_float3(fmaxf(0.f, rgb_linear_in.x), fmaxf(0.f, rgb_linear_in.y),
                                  fmaxf(0.f, rgb_linear_in.z));
  if (otf_type == CudaDrtEotf::LINEAR) {
    return rgb_linear;
  } else if (otf_type == CudaDrtEotf::ST2084) {
    return Y_to_ST2084_f3(rgb_linear);
  } else if (otf_type == CudaDrtEotf::HLG) {
    return HLG_from_display_linear_1000nits_f3(rgb_linear);
  } else if (otf_type == CudaDrtEotf::BT1886) {
    return bt1886_inv_f3(rgb_linear, 2.4f, 1.0f, 0.0f);
  } else if (otf_type == CudaDrtEotf::GAMMA_2_6) {
    return pow_f3(rgb_linear, 1.0f / 2.6f);
  } else if (otf_type == CudaDrtEotf::GAMMA_2_2) {
    return pow_f3(rgb_linear, 1.0f / 2.2f);
  } else if (otf_type == CudaDrtEotf::GAMMA_1_8) {
    return pow_f3(rgb_linear, 1.0f / 1.8f);
  } else {
    return moncurve_inv_f3(rgb_linear, 2.4f, 0.055f);
  }
}

GPU_FUNC float3 eotf(const float3& rgb_cv, CudaDrtEotf eotf_enum) {
  switch (eotf_enum) {
    case CudaDrtEotf::LINEAR:
      return rgb_cv;
    case CudaDrtEotf::ST2084:
      return mult_f_f3(ST2084_to_Y_f3(rgb_cv), 1.0f / ref_luminance);
    case CudaDrtEotf::HLG:
      return HLG_to_display_linear_1000nits_f3(rgb_cv);
    case CudaDrtEotf::BT1886:
      return pow_f3(rgb_cv, 2.6f);
    case CudaDrtEotf::GAMMA_2_6:
      return pow_f3(rgb_cv, 2.6f);
    case CudaDrtEotf::GAMMA_2_2:
      return pow_f3(rgb_cv, 2.2f);
    case CudaDrtEotf::GAMMA_1_8:
      return pow_f3(rgb_cv, 1.8f);
    default:
      return moncurve_fwd_f3(rgb_cv, 2.4f, 0.055f);
  }
}

GPU_FUNC float3 ApplyWhiteScale(float3& rgb, float* MAT_limit_to_display) {
  float3 RGB_w_f = mult_f3_f33(make_float3(1.f, 1.f, 1.f), MAT_limit_to_display);
  float  scale   = 1.f / fmaxf(fmaxf(RGB_w_f.x, RGB_w_f.y), RGB_w_f.z);

  return mult_f_f3(rgb, scale);
}

GPU_FUNC float3 DisplayEncoding(float3& rgb, float* MAT_limit_to_display, CudaDrtEotf eotf_num,
                                 float linear_scale = 1.f) {
  float3 rgb_disp_linear = mult_f3_f33(rgb, MAT_limit_to_display);
  float3 rgb_display_scaled =
      mult_f_f3(rgb_disp_linear, linear_scale);  // Scale to display luminance
  return eotf_inv(rgb_display_scaled, eotf_num);
}

GPU_FUNC float3 DisplayDecoding(float3& rgb_cv, float* MAT_display_to_limit, CudaDrtEotf eotf_num,
                                 float linear_scale = 1.f) {
  float3 rgb_display_linear = eotf(rgb_cv, eotf_num);
  float3 rgb_limit_scaled =
      mult_f_f3(rgb_display_linear, 1.f / linear_scale);  // Scale from display luminance
  return mult_f3_f33(rgb_limit_scaled, MAT_display_to_limit);
}

}  // namespace CUDA
}  // namespace alcedo
