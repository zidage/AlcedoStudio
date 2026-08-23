//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// CUDA implementations of helper functions in Lib.Academy.OutputTransform.ctl
// Reference:
// https://github.com/aces-aswf/aces-core/blob/dev/lib/Lib.Academy.OutputTransform.ctl

#pragma once

#include <cuda_runtime.h>
#include <math_constants.h>
#include <vector_functions.h>
#include <vector_types.h>

#include <cmath>

#include "edit/operators/GPU_kernels/color_mgmt/odt_const.cuh"
#include "edit/operators/GPU_kernels/param.cuh"
#include "odt_const.cuh"
#include "tonescale_funcs.cuh"
#include "util_funcs.cuh"

namespace alcedo {
namespace CUDA {

// ---- small CUDA-friendly "safe math" helpers (avoid NaN/Inf; keep fast paths) ----
GPU_FUNC bool isfinite_f(float x) {
#if defined(__CUDA_ARCH__)
  return isfinite(x);
#else
  return std::isfinite(x);
#endif
}

GPU_FUNC float clamp_f(float x, float lo, float hi) { return fminf(fmaxf(x, lo), hi); }

GPU_FUNC int clamp_i(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

GPU_FUNC float safe_pos(float x) { return fmaxf(x, 0.0f); }

GPU_FUNC float safe_sqrt(float x) { return sqrtf(fmaxf(x, 0.0f)); }

GPU_FUNC float safe_div(float a, float b, float eps = 1e-7f) {
  // Avoid 0/0 and huge blow-ups; keeps sign of b for continuity.
  const float ab = fabsf(b);
  const float bb = (ab < eps) ? copysignf(eps, b) : b;
  return a / bb;
}

GPU_FUNC float safe_log10_ratio(float num, float den, float eps = 1e-7f) {
  // log10(num/den) with both sides clamped away from 0.
  const float n = fmaxf(num, eps);
  const float d = fmaxf(den, eps);
  return log10f(n / d);
}

GPU_FUNC float safe_pow_pos(float base, float exp) {
  // pow for non-negative base; clamps base to >=0 to avoid NaNs for fractional exp.
  return powf(fmaxf(base, 0.0f), exp);
}

struct HueDependentGamutParams {
  // Hue-dependent gamut parameters
  float2 JMcusp;
  float  gamma_bottom_inv;
  float  gamma_top_inv;
  float  focus_J;
  float  analytical_threshold;
};
GPU_FUNC float3 clamp_AP1(const float3& AP1_color, float clamp_lower_limit,
                          float clamp_upper_limit) {
  // float3 AP1_color = mult_f3_f33(AP0_color, AP0_TO_AP1);
  float3 AP1_clamped = clamp_f3(AP1_color, clamp_lower_limit, clamp_upper_limit);
  float3 AP0_clamped = mult_f3_f33(AP1_clamped, AP1_TO_AP0);
  return AP0_clamped;
}

GPU_FUNC float table_get(const GPU_Table1D<float>& table, int index) {
  return tex1Dfetch<float>(table.texture_object_, index);
}

GPU_FUNC float wrap_to_360(float hue) {
  if (!isfinite_f(hue)) return 0.0f;
  float y   = fmodf(hue, 360.f);
  // branchless: add 360 when y<0, else add 0
  float add = ((float)(y < 0.f)) * 360.f;  // step(x,y)= y<=x?1:0 in CTL; adjust if different
  return y + add;
}

GPU_FUNC float radians_to_degrees(float rad) { return rad * (180.f / CUDART_PI_F); }

GPU_FUNC float degrees_to_radians(float deg) { return deg * (CUDART_PI_F / 180.f); }

GPU_FUNC int hue_position_in_uniform_table(float hue, int table_size) {
  const float wrapped = wrap_to_360(hue);  // [0, 360)
  const float pos     = wrapped * ((float)table_size / hue_limit);
  // trunc == floor for positive; clamp to valid range.
  const int   idx     = (int)pos;
  return clamp_i(idx, 0, table_size - 1);
}

GPU_FUNC float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

// reach_M_from_table(h, p.TABLE_reach_M)
GPU_FUNC float reach_M_from_table(float h, const GPU_Table1D<float>& table) {
  const float hw   = wrap_to_360(h);
  const float pos  = hw * ((float)tableSize / hue_limit);
  const int   base = clamp_i((int)pos, 0, tableSize - 1);
  const float t    = clamp_f(pos - (float)base, 0.0f, 1.0f);

  const int   i_lo = base + baseIndex;  // baseIndex=1 (padding)
  const int   i_hi = i_lo + 1;

  //  GPU_Table1D is cudaResourceTypeLinear + point fetch, so use tex1Dfetch
  const float lo   = tex1Dfetch<float>(table.texture_object_, i_lo);
  const float hi   = tex1Dfetch<float>(table.texture_object_, i_hi);

  return lerp_f(lo, hi, t);
}

GPU_FUNC float reach_M_from_table(float h, GPU_ODTParams& p) {
  return reach_M_from_table(h, p.table_reach_M_);
}

GPU_FUNC float _pacrc_fwd(float Rc) {
  const float F_L_Y = powf(Rc, 0.42f);
  return (F_L_Y) / (cam_nl_offset + F_L_Y);
}

GPU_FUNC float pacrc_fwd(float v) {
  const float abs_v = fabsf(v);
  const float Rc    = _pacrc_fwd(abs_v);
  return copysignf(Rc, v);
}

GPU_FUNC float _pacrc_inv(float Ra) {
  const float Ra_lim = fminf(Ra, 0.99f);
  const float F_L_Y  = (cam_nl_offset * Ra_lim) / (1.f - Ra_lim);
  return powf(F_L_Y, (1.f / 0.42f));
}

GPU_FUNC float pacrc_inv(float v) {
  const float abs_v = fabsf(v);
  const float Rc    = _pacrc_inv(abs_v);
  return copysignf(Rc, v);
}

GPU_FUNC float Achromatic_n_to_J(float A, float cz) { return J_scale * powf(A, cz); }

GPU_FUNC float J_to_Achromatic_n(float J, float inv_cz) { return powf(J * (1. / J_scale), inv_cz); }

GPU_FUNC float3 RGB_to_Aab(const float3& RGB, GPU_JMhParams& p) {
  float3 RGB_m = mult_f3_f33(RGB, p.MATRIX_RGB_to_CAM16_c_);

  float3 RGB_a = make_float3(pacrc_fwd(RGB_m.x), pacrc_fwd(RGB_m.y), pacrc_fwd(RGB_m.z));

  float3 Aab   = mult_f3_f33(RGB_a, p.MATRIX_cone_response_to_Aab_);
  return Aab;
}

GPU_FUNC float3 Aab_to_JMh(const float3& Aab, GPU_JMhParams& p) {
  const float mask  = Aab.x > 0.f ? 1.f : 0.f;
  const float J     = Achromatic_n_to_J(Aab.x, p.cz_) * mask;
  const float M2    = Aab.y * Aab.y + Aab.z * Aab.z;
  const float M     = safe_sqrt(M2) * mask;

  const float h_rad = atan2f(Aab.z, Aab.y);
  const float h     = wrap_to_360(radians_to_degrees(h_rad)) * mask;

  return make_float3(J, M, h);
}

GPU_FUNC float3 JMh_to_Aab(const float3& JMh, GPU_JMhParams& p) {
  float J      = JMh.x;
  float M      = JMh.y;
  float h      = JMh.z;
  float h_rad  = degrees_to_radians(h);
  float cos_hr = cosf(h_rad);
  float sin_hr = sinf(h_rad);

  float A      = J_to_Achromatic_n(J, p.inv_cz_);
  float a      = M * cos_hr;
  float b      = M * sin_hr;
  return make_float3(A, a, b);
}

GPU_FUNC float3 Aab_to_RGB(const float3& Aab, GPU_JMhParams& p) {
  float3 RGB_a = mult_f3_f33(Aab, p.MATRIX_Aab_to_cone_response_);

  float3 RGB_m = make_float3(pacrc_inv(RGB_a.x), pacrc_inv(RGB_a.y), pacrc_inv(RGB_a.z));

  float3 RGB   = mult_f3_f33(RGB_m, p.MATRIX_CAM16_c_to_RGB_);
  return RGB;
}

GPU_FUNC float3 RGB_to_JMh(const float3& color, GPU_JMhParams& p) {
  float3 Aab = RGB_to_Aab(color, p);
  return Aab_to_JMh(Aab, p);
}

GPU_FUNC float3 JMh_to_RGB(const float3& JMh, GPU_JMhParams& p) {
  float3 Aab = JMh_to_Aab(JMh, p);
  return Aab_to_RGB(Aab, p);
}

GPU_FUNC float _A_to_Y(float A, GPU_JMhParams& p) {
  float Ra = p.A_w_J_ * A;
  float Y  = _pacrc_inv(Ra) / p.F_L_n_;

  return Y;
}

GPU_FUNC float J_to_Y(float J, GPU_JMhParams& p) {
  float abs_J = fabsf(J);
  return _A_to_Y(J_to_Achromatic_n(abs_J, p.inv_cz_), p);
}

GPU_FUNC float Y_to_J(float Y, GPU_JMhParams& p) {
  float abs_Y = fabsf(Y);
  float Ra    = _pacrc_fwd(abs_Y * p.F_L_n_);
  float J     = Achromatic_n_to_J(Ra * p.inv_A_w_J_, p.cz_);

  return copysignf(J, Y);
}

GPU_FUNC float chroma_compress_norm(float h, float chroma_compress_scale) {
  float hr      = degrees_to_radians(h);

  float a       = cosf(hr);
  float b       = sinf(hr);
  float cos_hr2 = a * a - b * b;
  float sin_hr2 = 2.f * a * b;
  float cos_hr3 = 4.f * a * a * a - 3.f * a;
  float sin_hr3 = 3.f * b - 4.f * b * b * b;

  float M       = 11.34072f * a + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 + 14.66441f * b +
            -6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f;

  return M * chroma_compress_scale;
}

GPU_FUNC float reinhard_remap(float scale, float nd, bool invert = false) {
  const float fwd = scale * nd / (1.0f + nd);

  if (invert) {
    if (nd >= 1.0f) {
      return scale;
    } else {
      return scale * -(nd / (nd - 1.0f));
    }
  }

  return fwd;
}

// A "toe" function that remaps the given value x between 0 and limit.
// The k1 and k2 parameters change the size and shape of the toe.
// https://www.desmos.com/calculator/6vplvw14ti
GPU_FUNC float toe(float x, float limit, float k1_in, float k2_in, int invert = 0 /* 0 or 1 */
) {
  // Clamp k2 to avoid division by 0 / extreme slope
  if (x > limit) return x;

  float k2 = fmaxf(k2_in, 0.001f);
  float k1 = sqrtf(k1_in * k1_in + k2 * k2);
  float k3 = (limit + k1) / (limit + k2);

  if (invert) {
    return (x * x + k1 * x) / (k3 * (x + k2));
  } else {
    const float minus_b = k3 * x - k1;
    const float minus_c = k2 * k3 * x;
    return 0.5f * (minus_b + safe_sqrt(minus_b * minus_b + 4.f * minus_c));
  }
}

GPU_FUNC float3 chroma_compress_fwd(const float3& JMh, float tonemapped_J, GPU_ODTParams& p,
                                    bool invert = false) {
  float J       = JMh.x;
  float M       = JMh.y;
  float h       = JMh.z;

  float M_compr = M;

  if (M != 0.f) {
    const float limitJ = fmaxf(p.limit_J_max, 1e-6f);
    const float Jts    = fmaxf(tonemapped_J, 0.0f);
    const float nJ     = clamp_f(Jts / limitJ, 0.0f, 1.0f);
    const float snJ   = fmaxf(0.f, 1.f - nJ);
    float       Mnorm = chroma_compress_norm(h, p.chroma_compress_scale);
    if (!isfinite_f(Mnorm) || fabsf(Mnorm) < 1e-6f) {
      // Degenerate normalization: avoid division by ~0.
      return make_float3(Jts, 0.0f, h);
    }
    float limit     = safe_pow_pos(nJ, p.model_gamma_inv) * reach_M_from_table(h, p.table_reach_M_) / Mnorm;
    limit           = fmaxf(limit, 0.0f);

    float toe_limit = limit - 0.001f;
    float toe_snJ_sat         = snJ * p.sat;
    float toe_sqrt_nJ_sat_thr = sqrtf(nJ * nJ + p.sat_thr);
    float toe_nJ_compr        = nJ * p.compr;

    // Rescaling of M with the tonescaled J to get the M to the same range as
    // J after the tonescale.  The rescaling uses the Hellwig2022 model gamma to
    // keep the M/J ratio correct (keeping the chromaticities constant).
    // Avoid division by ~0 in near-black. If J is tiny, keep ratio ~1 to prevent blow-ups.
    const float absJ          = fabsf(J);
    const float ratio         = (absJ < 1e-6f) ? 1.0f : (Jts / absJ);
    M_compr                   = M * safe_pow_pos(ratio, p.model_gamma_inv);

    // Normalize M with the rendering space cusp M
    M_compr                   = M_compr / Mnorm;

    // Expand the colorfulness by running the toe function in reverse.  The goal is to
    // expand less saturated colors less and more saturated colors more.  The expansion
    // increases saturation in the shadows and mid-tones but not in the highlights.
    // The 0.001 offset starts the expansions slightly above zero.  The sat_thr makes
    // the toe less aggressive near black to reduce the expansion of noise.
    M_compr = limit - toe(limit - M_compr, toe_limit, toe_snJ_sat, toe_sqrt_nJ_sat_thr, false);

    // Compress the colorfulness.  The goal is to compress less saturated colors more and
    // more saturated colors less, especially in the highlights.  This step creates the
    // saturation roll-off in the highlights, but attemps to preserve pure colors.  This
    // mostly affects highlights and mid-tones, and does not compress shadows.
    M_compr = toe(M_compr, limit, toe_nJ_compr, snJ, false);

    // Denormalize M
    M_compr = M_compr * Mnorm;
  }

  return make_float3(tonemapped_J, M_compr, h);
}

GPU_FUNC float3 tonemap_and_compress_fwd(const float3& JMh, GPU_ODTParams& p) {
  // Applies the forward tonescale, then compresses M based on J and tonemapped J

  // Tonemap
  float linear       = J_to_Y(JMh.x, p.input_params_) / ref_luminance;

  float tonemapped_Y = Tonescale_fwd(linear, p.ts_);

  float J_ts         = Y_to_J(tonemapped_Y, p.input_params_);

  // Compress M; functio returns { tonemapped J, compressed M, h }
  return chroma_compress_fwd(JMh, J_ts, p);
  // return make_float3(J_ts, JMh.y, JMh.z);
}

GPU_FUNC int look_hue_interval(float h, const GPU_Table1D<float>& hue_table,
                               int* hue_linearity_search_range) {
  const float hw = wrap_to_360(h);
  int         i  = baseIndex + hue_position_in_uniform_table(hw, tableSize);  //  tableSize=360
  int         i_lo = i + hue_linearity_search_range[0];
  int         i_hi = i + hue_linearity_search_range[1];

  // clamp to valid padded range [baseIndex, baseIndex + tableSize]
  i_lo             = i_lo < baseIndex ? baseIndex : i_lo;
  i_hi             = i_hi > (baseIndex + tableSize) ? (baseIndex + tableSize) : i_hi;

#pragma unroll
  for (int k = 0; k < 6 && (i_lo + 1 < i_hi); ++k) {  // log2(range) <= 6
    const float v  = tex1Dfetch<float>(hue_table.texture_object_, i);
    const int   gt = hw > v;  // 0/1
    i_lo           = gt ? i : i_lo;
    i_hi           = gt ? i_hi : i;
    i              = (i_lo + i_hi) >> 1;  // midpoint
  }

  return (i_hi < 1) ? 1 : i_hi;
}

GPU_FUNC float  interpolation_weight(float h, float h_lo, float h_hi) { return h - h_lo; }

GPU_FUNC float2 cusp_from_table(float h, const GPU_Table1D<float4>& table) {
  const float hw     = wrap_to_360(h);

  int         low_i  = 0;
  int         high_i = baseIndex + tableSize;
  int         i      = baseIndex + hue_position_in_uniform_table(hw, tableSize);

#pragma unroll
  for (int k = 0; k < 10 && (low_i + 1 < high_i); ++k) {  // log2(362) < 10
    const float h_i = tex1Dfetch<float4>(table.texture_object_, i).z;
    const int   gt  = hw > h_i;  // 0/1 mask to reduce divergence
    low_i           = gt ? i : low_i;
    high_i          = gt ? high_i : i;
    i               = (low_i + high_i) >> 1;  // midpoint
  }

  const float4 lo    = tex1Dfetch<float4>(table.texture_object_, high_i - 1);
  const float4 hi    = tex1Dfetch<float4>(table.texture_object_, high_i);
  const float  denom = hi.z - lo.z;
  const float  t     = (denom != 0.0f) ? (hw - lo.z) / denom : 0.0f;

  return make_float2(lerp_f(lo.x, hi.x, t), lerp_f(lo.y, hi.y, t));
}

GPU_FUNC float2 cusp_from_table(float h, GPU_ODTParams& p) {
  return cusp_from_table(h, p.table_gamut_cusps_);
}

GPU_FUNC float compute_focus_J(float cusp_J, float mid_J, float limit_J_max) {
  return lerp_f(cusp_J, mid_J, fminf(1.f, cusp_mid_blend - (cusp_J / limit_J_max)));
}

GPU_FUNC HueDependentGamutParams init_HueDependentGamutParams(float h, GPU_ODTParams& p) {
  HueDependentGamutParams hdp;
  hdp.gamma_bottom_inv = p.lower_hull_gamma_inv;

  const int   i_hi     = look_hue_interval(h, p.table_hues_, p.hue_linearity_search_range);
  const float hw = wrap_to_360(h);
  const float t  = interpolation_weight(hw, table_get(p.table_hues_, i_hi - 1), table_get(p.table_hues_, i_hi));

  hdp.JMcusp               = cusp_from_table(h, p.table_gamut_cusps_);
  hdp.gamma_top_inv        = lerp_f(table_get(p.table_upper_hull_gamma_, i_hi - 1),
                                    table_get(p.table_upper_hull_gamma_, i_hi), t);
  hdp.focus_J              = compute_focus_J(hdp.JMcusp.x, p.mid_J, p.limit_J_max);
  hdp.analytical_threshold = lerp_f(hdp.JMcusp.x, p.limit_J_max, focus_gain_blend);

  return hdp;
}

GPU_FUNC float get_focus_gain(float J, float analytical_threshold, float limit_J_max,
                              float focus_dist) {
  float gain = limit_J_max * focus_dist;

  if (J > analytical_threshold) {
    // Approximate inverse required above threshold due to the introduction of J in the calculation
    const float num = limit_J_max - analytical_threshold;
    const float den = limit_J_max - J;
    float gain_adjustment = safe_log10_ratio(num, den, 1e-4f);
    gain_adjustment = gain_adjustment * gain_adjustment + 1.f;
    gain            = gain * gain_adjustment;
  }

  return gain;
}

GPU_FUNC float solve_J_intersect(float J, float M, float focusJ, float maxJ, float slope_gain) {
  const float sg       = fmaxf(fabsf(slope_gain), 1e-6f);
  const float fj       = fmaxf(fabsf(focusJ), 1e-6f);
  const float M_scaled = M / sg;
  const float a        = M_scaled / fj;

  // branch 1: J < focusJ
  const float b1       = 1.f - M_scaled;
  const float c1       = -J;
  const float det1     = b1 * b1 - 4.f * a * c1;
  const float r1       = (det1 > 0.f) ? safe_sqrt(det1) : 0.f;
  const float den1     = copysignf(fmaxf(fabsf(b1 + r1), 1e-6f), (b1 + r1));
  const float res1     = (-2.f * c1) / den1;

  // branch 2: J >= focusJ
  const float b2       = -(1.f + M_scaled + maxJ * a);
  const float c2       = maxJ * M_scaled + J;
  const float det2     = b2 * b2 - 4.f * a * c2;
  const float r2       = (det2 > 0.f) ? safe_sqrt(det2) : 0.f;
  const float den2     = copysignf(fmaxf(fabsf(b2 - r2), 1e-6f), (b2 - r2));
  const float res2     = (-2.f * c2) / den2;

  const int   choose1  = J < focusJ;  // 0/1 mask
  return choose1 ? res1 : res2;
}

GPU_FUNC float compute_compression_vector_slope(float intersect_J, float focus_J, float limit_J_max,
                                                float slope_gain) {
  // mask: 1 if intersect_J < focus_J else 0
  const int   m                = intersect_J < focus_J;
  const float dir_lo           = intersect_J;
  const float dir_hi           = limit_J_max - intersect_J;
  const float direction_scalar = m ? dir_lo : dir_hi;

  const float denom = fmaxf(fabsf(focus_J * slope_gain), 1e-6f);
  return direction_scalar * (intersect_J - focus_J) / denom;
}

GPU_FUNC float estimate_line_and_boundary_intersection_M(float J_axis_intersect, float slope,
                                                         float inv_gamma, float J_max, float M_max,
                                                         float J_intersection_reference) {
  // Line defined by     J = slope * x + J_axis_intersect
  // Boundary defined by J = J_max * (x / M_max) ^ (1/inv_gamma)
  // Approximate as we do not want to iteratively solve intersection of a
  // straight line and an exponential

  // We calculate a shifted intersection from the original intersection using
  // the inverse of the exponential and the provided reference
  const float refJ                 = fmaxf(J_intersection_reference, 1e-6f);
  const float normalised_J         = fmaxf(J_axis_intersect / refJ, 0.0f);
  const float shifted_intersection = refJ * safe_pow_pos(normalised_J, inv_gamma);

  // Now we find the M intersection of two lines
  // line from origin to J,M Max       l1(x) = J/M * x
  // line from J Intersect' with slope l2(x) = slope * x + Intersect'

  // return shifted_intersection / ((J_max / M_max) - slope);
  const float denom = copysignf(fmaxf(fabsf(J_max - slope * M_max), 1e-6f), (J_max - slope * M_max));
  return shifted_intersection * M_max / denom;
}

// Smooth minimum about the scaled reference, based upon a cubic polynomial
GPU_FUNC float smin_scaled(float a, float b, float scale_reference) {
  const float s_scaled = smooth_cusps * scale_reference;
  if (s_scaled <= 1e-6f) return fminf(a, b);
  const float h        = fmaxf(s_scaled - fabsf(a - b), 0.0f) / s_scaled;
  return fminf(a, b) - h * h * h * s_scaled * (1. / 6.);
}

GPU_FUNC float find_gamut_boundary_intersection(float2 JM_cusp, float J_max, float gamma_top_inv,
                                                float gamma_bottom_inv, float J_intersect_source,
                                                float slope, float J_intersect_cusp) {
  const float M_boundary_lower = estimate_line_and_boundary_intersection_M(
      J_intersect_source, slope, gamma_bottom_inv, JM_cusp.x, JM_cusp.y, J_intersect_cusp);
  // The upper hull is flipped and thus 'zeroed' at J_max
  // Also note we negate the slope
  const float f_J_intersect_cusp   = J_max - J_intersect_cusp;
  const float f_J_intersect_source = J_max - J_intersect_source;
  const float f_JM_cusp_J          = J_max - JM_cusp.x;
  const float M_boundary_upper     = estimate_line_and_boundary_intersection_M(
      f_J_intersect_source, -slope, gamma_top_inv, f_JM_cusp_J, JM_cusp.y, f_J_intersect_cusp);

  // Smooth minimum between the two calculated values for the M component
  float M_boundary = smin_scaled(M_boundary_lower, M_boundary_upper, JM_cusp.y);
  return M_boundary;
}

GPU_FUNC float remap_M(float M, float gamut_boundary_M, float reach_boundary_M,
                       bool invert = false) {
  const float boundary_ratio = safe_div(gamut_boundary_M, reach_boundary_M, 1e-6f);
  const float proportion     = fmaxf(boundary_ratio, compression_threshold);
  const float threshold      = proportion * gamut_boundary_M;

  if (M <= threshold || proportion >= 1.f) return M;

  const float m_offset  = M - threshold;
  const float gamut_off = gamut_boundary_M - threshold;
  const float reach_off = reach_boundary_M - threshold;
  const float ratio_rg  = safe_div(reach_off, gamut_off, 1e-6f);
  const float denom     = fmaxf(ratio_rg - 1.f, 1e-7f);
  const float scale     = reach_off / denom;
  const float nd        = m_offset / scale;

  return threshold + reinhard_remap(scale, nd, invert);
}

GPU_FUNC float3 compress_gamut(const float3& JMh, float Jx, GPU_ODTParams& p,
                               HueDependentGamutParams& hdp, bool invert = false) {
  const float J = JMh.x;
  const float M = JMh.y;
  const float h = JMh.z;

  const float slope_gain = fmaxf(get_focus_gain(Jx, hdp.analytical_threshold, p.limit_J_max, p.focus_dist), 1e-6f);
  const float J_intersect_source = solve_J_intersect(J, M, hdp.focus_J, p.limit_J_max, slope_gain);
  const float gamut_slope =
      compute_compression_vector_slope(J_intersect_source, hdp.focus_J, p.limit_J_max, slope_gain);

  const float J_intersect_cusp =
      solve_J_intersect(hdp.JMcusp.x, hdp.JMcusp.y, hdp.focus_J, p.limit_J_max, slope_gain);

  const float gamut_boundary_M = find_gamut_boundary_intersection(
      hdp.JMcusp, p.limit_J_max, hdp.gamma_top_inv, hdp.gamma_bottom_inv, J_intersect_source,
      gamut_slope, J_intersect_cusp);

  if (gamut_boundary_M <= 0.f) {
    return make_float3(Jx, 0.f, h);
  }

  float       reach_max_M = fmaxf(reach_M_from_table(h, p), 0.0f);

  const float reach_boundary_M =
      estimate_line_and_boundary_intersection_M(J_intersect_source, gamut_slope, p.model_gamma_inv,
                                                p.limit_J_max, reach_max_M, p.limit_J_max);

  const float remapped_M = remap_M(M, gamut_boundary_M, reach_boundary_M, invert);
  return make_float3(J_intersect_source + remapped_M * gamut_slope, remapped_M, h);
}

GPU_FUNC float3 gamut_compress_fwd(const float3& JMh, GPU_ODTParams& p) {
  const float J = JMh.x;
  const float M = JMh.y;
  const float h = JMh.z;

  if (J <= 0.f) {
    return make_float3(0.f, 0.f, h);
  }

  if (M <= 0.f || J > p.limit_J_max) {
    return make_float3(J, 0.f, h);
  }
  HueDependentGamutParams hdp = init_HueDependentGamutParams(h, p);
  return compress_gamut(JMh, J, p, hdp, false);
}

// --- add: chroma-preserving limiter (scale RGB uniformly when exceeding upper) ---
GPU_FUNC float3 limit_rgb_preserve_chroma(float3 rgb, float lower, float upper) {
  // Guard NaN/Inf: keep behavior predictable in later pow/log.
  if (!isfinite(rgb.x) || !isfinite(rgb.y) || !isfinite(rgb.z)) {
    return make_float3(0.f, 0.f, 0.f);
  }

  // Lower clamp (negatives usually break perceptual ops; keep per-channel for safety)
  rgb.x = fmaxf(rgb.x, lower);
  rgb.y = fmaxf(rgb.y, lower);
  rgb.z = fmaxf(rgb.z, lower);

  // Upper limit: preserve ratios by scaling uniformly
  const float m = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
  if (m > upper && m > 0.f) {
    const float s = upper / m;
    rgb.x *= s;
    rgb.y *= s;
    rgb.z *= s;
  }
  return rgb;
}

GPU_FUNC float3 OutputTransform_fwd(const float3& in_color, GPU_ODTParams& p) {
  if (!isfinite_f(in_color.x) || !isfinite_f(in_color.y) || !isfinite_f(in_color.z)) {
    return make_float3(0.f, 0.f, 0.f);
  }
  float3 AP0 = mult_f3_f33(in_color, AP1_TO_AP0);

  float3 JMh            = RGB_to_JMh(AP0, p.input_params_);
  float3 tonemapped_JMh = tonemap_and_compress_fwd(JMh, p);
  float3 compressed_JMh = gamut_compress_fwd(tonemapped_JMh, p);
  float3 out_rgb = JMh_to_RGB(compressed_JMh, p.limit_params_);
  // Keep output finite and within forward range, preserving chroma when exceeding upper.
  return limit_rgb_preserve_chroma(out_rgb, 0.0f, p.ts_.forward_limit_);
}
};  // namespace CUDA
};  // namespace alcedo