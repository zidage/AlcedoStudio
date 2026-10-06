// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Portions of this file are a port of OpenColorIO 2.5.1
// (src/OpenColorIO/ops/fixedfunction/ACES2/Transform.cpp and FixedFunctionOpGPU.cpp):
// Copyright Contributors to the OpenColorIO Project. SPDX-License-Identifier: BSD-3-Clause.
// See THIRD_PARTY_NOTICE.txt.
#ifndef ALCEDO_ACES2_REFERENCE_MATH_H
#define ALCEDO_ACES2_REFERENCE_MATH_H

// The OpenColorIO 2.5.1 ACES 2.0 output transform (Renderer_ACES_OutputTransform20::fwd and
// ::inv) with the closed-form gamut boundary of the OCIO GPU shader generator, shared by the host,
// CUDA, OpenCL C and Metal (docs/roadmap/alcedo_studio/edit/lut_color_encoding_plan.md, section
// 7). This is the reference transform, not Alcedo's own ACES 2.0 DRT (odt_funcs.cuh), which
// differs from it on purpose.
//
// Both directions read one float block with the layout below. The host fills it
// (aces2_reference_runtime.cpp) for one display primaries and peak luminance. Display RGB is
// linear, 1.0 = 100 nits, in the display primaries; scene RGB is ACES2065-1 (AP0).
//
// OpenCL programs list this file before display_to_ap1_math.h; Metal shaders include it first.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define A2R_GLOBAL __global
#define A2R_INLINE static inline
#define A2R_POW    pow
#define A2R_SQRT   sqrt
#define A2R_ATAN2  atan2
#define A2R_COS    cos
#define A2R_SIN    sin
#define A2R_LOG10  log10
#define A2R_FABS   fabs
#define A2R_FLOOR  floor
#elif defined(__METAL_VERSION__)
#define A2R_GLOBAL device
#define A2R_INLINE static inline
#define A2R_POW    pow
#define A2R_SQRT   sqrt
#define A2R_ATAN2  atan2
#define A2R_COS    cos
#define A2R_SIN    sin
#define A2R_LOG10  log10
#define A2R_FABS   fabs
#define A2R_FLOOR  floor
#else
#include <cmath>
#define A2R_GLOBAL
#ifdef __CUDACC__
#define A2R_INLINE static __host__ __device__ inline
#else
#define A2R_INLINE static inline
#endif
#define A2R_POW   powf
#define A2R_SQRT  sqrtf
#define A2R_ATAN2 atan2f
#define A2R_COS   cosf
#define A2R_SIN   sinf
#define A2R_LOG10 log10f
#define A2R_FABS  fabsf
#define A2R_FLOOR floorf
#endif

// ---------------------------------------------------------------------------------------------
// Packed parameter layout (float indices)
// ---------------------------------------------------------------------------------------------

/// Always 0 in a reference block. display_to_ap1_math.h reads it as its branch flag.
#define ALCEDO_A2R_RESERVED               0
/// Display RGB to limiting RGB: identity, or display to AP1 (CAT02) when a display primary lies
/// outside AP1 and AP1 is the limiting gamut.
#define ALCEDO_A2R_DISPLAY_TO_LIMIT       1
#define ALCEDO_A2R_LIMIT_RGB_TO_CAM16     10
#define ALCEDO_A2R_LIMIT_CONE_TO_AAB      19
#define ALCEDO_A2R_AP0_AAB_TO_CONE        28
#define ALCEDO_A2R_AP0_CAM16_TO_RGB       37
#define ALCEDO_A2R_AP0_TO_AP1             46
#define ALCEDO_A2R_SCALARS                55
/// Peak luminance / 100: the upper clamp of the inverse's limiting RGB input.
#define ALCEDO_A2R_INPUT_MAX              (ALCEDO_A2R_SCALARS + 0)
/// Forward limit of the tonescale: the upper clamp of AP1 after the inverse.
#define ALCEDO_A2R_AP1_MAX                (ALCEDO_A2R_SCALARS + 1)
#define ALCEDO_A2R_CZ                     (ALCEDO_A2R_SCALARS + 2)
#define ALCEDO_A2R_INV_CZ                 (ALCEDO_A2R_SCALARS + 3)
#define ALCEDO_A2R_AP0_A_W_J              (ALCEDO_A2R_SCALARS + 4)
#define ALCEDO_A2R_AP0_INV_A_W_J          (ALCEDO_A2R_SCALARS + 5)
#define ALCEDO_A2R_AP0_F_L_N              (ALCEDO_A2R_SCALARS + 6)
#define ALCEDO_A2R_TS_INVERSE_LIMIT       (ALCEDO_A2R_SCALARS + 7)
#define ALCEDO_A2R_TS_T_1                 (ALCEDO_A2R_SCALARS + 8)
#define ALCEDO_A2R_TS_S_2                 (ALCEDO_A2R_SCALARS + 9)
#define ALCEDO_A2R_TS_M_2                 (ALCEDO_A2R_SCALARS + 10)
#define ALCEDO_A2R_TS_G                   (ALCEDO_A2R_SCALARS + 11)
#define ALCEDO_A2R_LIMIT_J_MAX            (ALCEDO_A2R_SCALARS + 12)
#define ALCEDO_A2R_MODEL_GAMMA_INV        (ALCEDO_A2R_SCALARS + 13)
#define ALCEDO_A2R_SAT                    (ALCEDO_A2R_SCALARS + 14)
#define ALCEDO_A2R_SAT_THR                (ALCEDO_A2R_SCALARS + 15)
#define ALCEDO_A2R_COMPR                  (ALCEDO_A2R_SCALARS + 16)
#define ALCEDO_A2R_CHROMA_COMPRESS_SCALE  (ALCEDO_A2R_SCALARS + 17)
#define ALCEDO_A2R_MID_J                  (ALCEDO_A2R_SCALARS + 18)
#define ALCEDO_A2R_FOCUS_DIST             (ALCEDO_A2R_SCALARS + 19)
#define ALCEDO_A2R_LOWER_HULL_GAMMA_INV   (ALCEDO_A2R_SCALARS + 20)
#define ALCEDO_A2R_HUE_SEARCH_LO          (ALCEDO_A2R_SCALARS + 21)
#define ALCEDO_A2R_HUE_SEARCH_HI          (ALCEDO_A2R_SCALARS + 22)
/// Forward only: tonescale output scale n_r (OCIO ToneScaleParams::n_r).
#define ALCEDO_A2R_TS_N_R                 (ALCEDO_A2R_SCALARS + 23)
#define ALCEDO_A2R_SCALAR_COUNT           32
/// Hue tables: 360 nominal entries, 1 lower and 2 upper wrap entries (OCIO layout).
#define ALCEDO_A2R_TABLE_SIZE             363
#define ALCEDO_A2R_TABLE_BASE_INDEX       1
#define ALCEDO_A2R_TABLE_LOWER_WRAP_INDEX 0
#define ALCEDO_A2R_TABLE_UPPER_WRAP_INDEX 361
#define ALCEDO_A2R_REACH_TABLE            (ALCEDO_A2R_SCALARS + ALCEDO_A2R_SCALAR_COUNT)
#define ALCEDO_A2R_HUE_TABLE              (ALCEDO_A2R_REACH_TABLE + ALCEDO_A2R_TABLE_SIZE)
/// Interleaved cusp J, cusp M and upper hull gamma inverse per entry.
#define ALCEDO_A2R_CUSP_TABLE             (ALCEDO_A2R_HUE_TABLE + ALCEDO_A2R_TABLE_SIZE)
/// Forward-only matrices.
#define ALCEDO_A2R_AP0_RGB_TO_CAM16       (ALCEDO_A2R_CUSP_TABLE + 3 * ALCEDO_A2R_TABLE_SIZE)
#define ALCEDO_A2R_AP0_CONE_TO_AAB        (ALCEDO_A2R_AP0_RGB_TO_CAM16 + 9)
#define ALCEDO_A2R_LIMIT_AAB_TO_CONE      (ALCEDO_A2R_AP0_CONE_TO_AAB + 9)
#define ALCEDO_A2R_LIMIT_CAM16_TO_RGB     (ALCEDO_A2R_LIMIT_AAB_TO_CONE + 9)
/// Inverse of ALCEDO_A2R_DISPLAY_TO_LIMIT.
#define ALCEDO_A2R_LIMIT_TO_DISPLAY       (ALCEDO_A2R_LIMIT_CAM16_TO_RGB + 9)
#define ALCEDO_A2R_PACKED_SIZE            (ALCEDO_A2R_LIMIT_TO_DISPLAY + 9)

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

typedef struct {
  float x, y, z;
} A2rFloat3;

A2R_INLINE A2rFloat3 A2rMake3(float x, float y, float z) {
  A2rFloat3 v;
  v.x = x;
  v.y = y;
  v.z = z;
  return v;
}

A2R_INLINE float     A2rMin(float a, float b) { return a < b ? a : b; }
A2R_INLINE float     A2rMax(float a, float b) { return a > b ? a : b; }
A2R_INLINE float     A2rClamp(float x, float lo, float hi) { return A2rMin(hi, A2rMax(lo, x)); }
A2R_INLINE float     A2rSign(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }
A2R_INLINE float     A2rLerp(float a, float b, float t) { return (b - a) * t + a; }

A2R_INLINE A2rFloat3 A2rMul(A2R_GLOBAL const float* m, A2rFloat3 v) {
  return A2rMake3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                  m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

// ---------------------------------------------------------------------------------------------
// Hellwig CAM (OCIO ACES2 constants)
// ---------------------------------------------------------------------------------------------

#define A2R_CAM_NL_OFFSET 27.13f /* 0.2713 * 100 */
#define A2R_PI            3.14159265358979f

A2R_INLINE float A2rConeCompressFwd(float v) {
  const float f = A2R_POW(A2R_FABS(v), 0.42f);
  return A2rSign(v) * f / (A2R_CAM_NL_OFFSET + f);
}

A2R_INLINE float A2rConeCompressInv(float v) {
  const float lim = A2rMin(A2R_FABS(v), 0.99f);
  return A2rSign(v) * A2R_POW(A2R_CAM_NL_OFFSET * lim / (1.0f - lim), 1.0f / 0.42f);
}

/// RGB to Aab with the matrices at @p rgb_to_cam16 and @p cone_to_aab (OCIO RGB_to_Aab).
A2R_INLINE A2rFloat3 A2rRgbToAab(A2rFloat3 rgb, A2R_GLOBAL const float* rgb_to_cam16,
                                 A2R_GLOBAL const float* cone_to_aab) {
  const A2rFloat3 lms = A2rMul(rgb_to_cam16, rgb);
  const A2rFloat3 rgb_a =
      A2rMake3(A2rConeCompressFwd(lms.x), A2rConeCompressFwd(lms.y), A2rConeCompressFwd(lms.z));
  return A2rMul(cone_to_aab, rgb_a);
}

/// Aab to JMh (h in degrees, [0, 360)). Non-positive A is black (OCIO Aab_to_JMh).
A2R_INLINE A2rFloat3 A2rAabToJmh(A2rFloat3 aab, A2R_GLOBAL const float* p) {
  if (aab.x <= 0.0f) return A2rMake3(0.0f, 0.0f, 0.0f);
  const float J = 100.0f * A2R_POW(aab.x, p[ALCEDO_A2R_CZ]);
  const float M = (J == 0.0f) ? 0.0f : A2R_SQRT(aab.y * aab.y + aab.z * aab.z);
  float       h = (aab.y == 0.0f) ? 0.0f : A2R_ATAN2(aab.z, aab.y) * (180.0f / A2R_PI);
  h             = h - A2R_FLOOR(h / 360.0f) * 360.0f;
  h             = (h < 0.0f) ? h + 360.0f : h;
  return A2rMake3(J, M, h);
}

/// JMh to RGB with the matrices at @p aab_to_cone and @p cam16_to_rgb (OCIO JMh_to_Aab followed
/// by Aab_to_RGB).
A2R_INLINE A2rFloat3 A2rJmhToRgb(A2rFloat3 jmh, float cos_hr, float sin_hr,
                                 A2R_GLOBAL const float* aab_to_cone,
                                 A2R_GLOBAL const float* cam16_to_rgb, A2R_GLOBAL const float* p) {
  const A2rFloat3 aab =
      A2rMake3(A2R_POW(jmh.x * 0.01f, p[ALCEDO_A2R_INV_CZ]), jmh.y * cos_hr, jmh.y * sin_hr);
  const A2rFloat3 rgb_a = A2rMul(aab_to_cone, aab);
  const A2rFloat3 lms   = A2rMake3(A2rConeCompressInv(rgb_a.x), A2rConeCompressInv(rgb_a.y),
                                   A2rConeCompressInv(rgb_a.z));
  return A2rMul(cam16_to_rgb, lms);
}

// ---------------------------------------------------------------------------------------------
// Table lookups
// ---------------------------------------------------------------------------------------------

A2R_INLINE float A2rReachMaxM(float h, A2R_GLOBAL const float* p) {
  const float i_base = A2R_FLOOR(h);
  const int   i_lo   = (int)i_base + ALCEDO_A2R_TABLE_BASE_INDEX;
  const float lo     = p[ALCEDO_A2R_REACH_TABLE + i_lo];
  const float hi     = p[ALCEDO_A2R_REACH_TABLE + i_lo + 1];
  return A2rLerp(lo, hi, h - i_base);
}

/// Cusp J, cusp M and upper hull gamma inverse at hue @p h.
A2R_INLINE A2rFloat3 A2rCuspSample(float h, A2R_GLOBAL const float* p) {
  A2R_GLOBAL const float* hues = p + ALCEDO_A2R_HUE_TABLE;
  int                     i    = (int)h + ALCEDO_A2R_TABLE_BASE_INDEX;
  int                     i_lo = i + (int)p[ALCEDO_A2R_HUE_SEARCH_LO];
  int                     i_hi = i + (int)p[ALCEDO_A2R_HUE_SEARCH_HI];
  if (i_lo < ALCEDO_A2R_TABLE_LOWER_WRAP_INDEX) i_lo = ALCEDO_A2R_TABLE_LOWER_WRAP_INDEX;
  if (i_hi > ALCEDO_A2R_TABLE_UPPER_WRAP_INDEX) i_hi = ALCEDO_A2R_TABLE_UPPER_WRAP_INDEX;
  while (i_lo + 1 < i_hi) {
    if (h > hues[i]) {
      i_lo = i;
    } else {
      i_hi = i;
    }
    i = (i_lo + i_hi) / 2;
  }
  if (i_hi < 1) i_hi = 1;
  A2R_GLOBAL const float* lo = p + ALCEDO_A2R_CUSP_TABLE + 3 * (i_hi - 1);
  A2R_GLOBAL const float* hi = p + ALCEDO_A2R_CUSP_TABLE + 3 * i_hi;
  const float             t  = (h - hues[i_hi - 1]) / (hues[i_hi] - hues[i_hi - 1]);
  return A2rMake3(A2rLerp(lo[0], hi[0], t), A2rLerp(lo[1], hi[1], t), A2rLerp(lo[2], hi[2], t));
}

// ---------------------------------------------------------------------------------------------
// Gamut compression (both directions)
// ---------------------------------------------------------------------------------------------

A2R_INLINE float A2rFocusGain(float J, float cusp_j, float limit_j_max) {
  const float thr = A2rLerp(cusp_j, limit_j_max, 0.3f);
  if (J > thr) {
    float gain = (limit_j_max - thr) / A2rMax(0.0001f, limit_j_max - J);
    gain       = A2R_LOG10(gain);
    return gain * gain + 1.0f;
  }
  return 1.0f;
}

A2R_INLINE float A2rSolveJIntersect(float J, float M, float focus_j, float limit_j_max,
                                    float slope_gain) {
  const float m_scaled = M / slope_gain;
  const float a        = m_scaled / focus_j;
  if (J < focus_j) {
    const float b = 1.0f - m_scaled;
    const float c = -J;
    return -2.0f * c / (b + A2R_SQRT(b * b - 4.0f * a * c));
  }
  const float b = -(1.0f + m_scaled + limit_j_max * a);
  const float c = limit_j_max * m_scaled + J;
  return -2.0f * c / (b - A2R_SQRT(b * b - 4.0f * a * c));
}

/// Closed-form gamut boundary of the OCIO GPU shader generator.
A2R_INLINE float A2rGamutBoundaryM(A2rFloat3 cusp, float gamma_bottom_inv, float j_source,
                                   float j_cusp, float slope, float limit_j_max) {
  const float lower =
      j_cusp * A2R_POW(j_source / j_cusp, gamma_bottom_inv) / (cusp.x / cusp.y - slope);
  const float upper = cusp.y * (limit_j_max - j_cusp) *
                      A2R_POW((limit_j_max - j_source) / (limit_j_max - j_cusp), cusp.z) /
                      (slope * cusp.y + limit_j_max - cusp.x);
  const float s = 0.12f * cusp.y;
  const float h = A2rMax(s - A2R_FABS(lower - upper), 0.0f) / s;
  return A2rMin(lower, upper) - h * h * h * s * (1.0f / 6.0f);
}

/// OCIO remap_M: Reinhard compression of M above the threshold, or its inverse.
A2R_INLINE float A2rRemapM(float M, float gamut_boundary_m, float reach_boundary_m, int inverse) {
  const float proportion = A2rMax(gamut_boundary_m / reach_boundary_m, 0.75f);
  const float threshold  = proportion * gamut_boundary_m;
  if (proportion >= 1.0f || M <= threshold) return M;
  const float m_offset     = M - threshold;
  const float gamut_offset = gamut_boundary_m - threshold;
  const float reach_offset = reach_boundary_m - threshold;
  const float scale        = reach_offset / ((reach_offset / gamut_offset) - 1.0f);
  const float nd           = m_offset / scale;
  if (inverse == 0) return threshold + scale * nd / (1.0f + nd);
  if (nd >= 1.0f) return threshold + scale;
  return threshold + scale * -(nd / (nd - 1.0f));
}

/// OCIO compressGamut<inverse> for a JMh with positive M and J at most limit J max. @p jx is the
/// J that sets the focus gain: J itself in the forward direction, an estimate in the inverse.
A2R_INLINE A2rFloat3 A2rCompressGamut(A2rFloat3 jmh, float jx, A2rFloat3 cusp, float reach_max_m,
                                      int inverse, A2R_GLOBAL const float* p) {
  const float limit_j_max = p[ALCEDO_A2R_LIMIT_J_MAX];
  if (jmh.y <= 0.0f || jmh.x > limit_j_max) return A2rMake3(jmh.x, 0.0f, jmh.z);
  const float focus_j =
      A2rLerp(cusp.x, p[ALCEDO_A2R_MID_J], A2rMin(1.0f, 1.3f - cusp.x / limit_j_max));
  const float slope_gain =
      limit_j_max * p[ALCEDO_A2R_FOCUS_DIST] * A2rFocusGain(jx, cusp.x, limit_j_max);
  const float j_source = A2rSolveJIntersect(jmh.x, jmh.y, focus_j, limit_j_max, slope_gain);
  float       slope    = (j_source < focus_j) ? j_source : (limit_j_max - j_source);
  slope                = slope * (j_source - focus_j) / (focus_j * slope_gain);
  const float j_cusp   = A2rSolveJIntersect(cusp.x, cusp.y, focus_j, limit_j_max, slope_gain);
  const float gamut_boundary_m = A2rGamutBoundaryM(cusp, p[ALCEDO_A2R_LOWER_HULL_GAMMA_INV],
                                                   j_source, j_cusp, slope, limit_j_max);
  if (gamut_boundary_m <= 0.0f) return A2rMake3(jmh.x, 0.0f, jmh.z);
  float reach_boundary_m =
      limit_j_max * A2R_POW(j_source / limit_j_max, p[ALCEDO_A2R_MODEL_GAMMA_INV]);
  reach_boundary_m       = reach_boundary_m / ((limit_j_max / reach_max_m) - slope);
  const float remapped_m = A2rRemapM(jmh.y, gamut_boundary_m, reach_boundary_m, inverse);
  return A2rMake3(j_source + remapped_m * slope, remapped_m, jmh.z);
}

// ---------------------------------------------------------------------------------------------
// Tonescale and chroma compression (both directions)
// ---------------------------------------------------------------------------------------------

/// Forward tonescale from the achromatic response A of the scene color to the tonemapped J
/// (OCIO tonescale_A_to_J_fwd). @p a must be positive.
A2R_INLINE float A2rTonescaleForwardFromA(float a, A2R_GLOBAL const float* p) {
  const float f_l_n = p[ALCEDO_A2R_AP0_F_L_N];
  // A -> Y (nits): _A_to_Y.
  const float y_in  = A2rConeCompressInv(p[ALCEDO_A2R_AP0_A_W_J] * a) / f_l_n;
  // aces_tonescale<forward>.
  const float f =
      p[ALCEDO_A2R_TS_M_2] * A2R_POW(y_in / (y_in + p[ALCEDO_A2R_TS_S_2]), p[ALCEDO_A2R_TS_G]);
  const float y_ts = A2rMax(0.0f, f * f / (f + p[ALCEDO_A2R_TS_T_1])) * p[ALCEDO_A2R_TS_N_R];
  // Y -> J: _Y_to_J.
  const float ra   = A2rConeCompressFwd(y_ts * f_l_n);
  return 100.0f * A2R_POW(ra * p[ALCEDO_A2R_AP0_INV_A_W_J], p[ALCEDO_A2R_CZ]);
}

/// Inverse tonescale from the tonemapped J to the scene J (OCIO tonescale_inv).
A2R_INLINE float A2rTonescaleInverse(float J, A2R_GLOBAL const float* p) {
  const float a_w_j = p[ALCEDO_A2R_AP0_A_W_J];
  const float f_l_n = p[ALCEDO_A2R_AP0_F_L_N];
  // J -> Y (nits): _J_to_Y.
  const float A     = A2R_POW(A2R_FABS(J) * 0.01f, p[ALCEDO_A2R_INV_CZ]);
  const float y_in  = A2rConeCompressInv(a_w_j * A) / f_l_n;
  // Closed-form inverse tonescale: aces_tonescale<inverse>.
  const float Z     = A2rMax(0.0f, A2rMin(p[ALCEDO_A2R_TS_INVERSE_LIMIT], y_in * 0.01f));
  const float f     = (Z + A2R_SQRT(Z * (4.0f * p[ALCEDO_A2R_TS_T_1] + Z))) * 0.5f;
  const float Y =
      p[ALCEDO_A2R_TS_S_2] / (A2R_POW(p[ALCEDO_A2R_TS_M_2] / f, 1.0f / p[ALCEDO_A2R_TS_G]) - 1.0f);
  // Y -> J: _Y_to_J.
  const float ra    = A2rConeCompressFwd(A2R_FABS(Y) * f_l_n);
  const float j_out = 100.0f * A2R_POW(ra * p[ALCEDO_A2R_AP0_INV_A_W_J], p[ALCEDO_A2R_CZ]);
  return A2rSign(J) * j_out;
}

A2R_INLINE float A2rToeForward(float x, float limit, float k1_in, float k2_in) {
  if (x > limit) return x;
  const float k2       = A2rMax(k2_in, 0.001f);
  const float k1       = A2R_SQRT(k1_in * k1_in + k2 * k2);
  const float k3       = (limit + k1) / (limit + k2);
  const float minus_b  = k3 * x - k1;
  const float minus_ac = k2 * k3 * x;
  return 0.5f * (minus_b + A2R_SQRT(minus_b * minus_b + 4.0f * minus_ac));
}

A2R_INLINE float A2rToeInverse(float x, float limit, float k1_in, float k2_in) {
  if (x > limit) return x;
  const float k2 = A2rMax(k2_in, 0.001f);
  const float k1 = A2R_SQRT(k1_in * k1_in + k2 * k2);
  const float k3 = (limit + k1) / (limit + k2);
  return (x * x + k1 * x) / (k3 * (x + k2));
}

A2R_INLINE float A2rChromaCompressNorm(float cos_hr, float sin_hr, float scale) {
  const float cos_hr2 = 2.0f * cos_hr * cos_hr - 1.0f;
  const float sin_hr2 = 2.0f * cos_hr * sin_hr;
  const float cos_hr3 = 4.0f * cos_hr * cos_hr * cos_hr - 3.0f * cos_hr;
  const float sin_hr3 = 3.0f * sin_hr - 4.0f * sin_hr * sin_hr * sin_hr;
  const float M       = 11.34072f * cos_hr + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 +
                  14.66441f * sin_hr + -6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f;
  return M * scale;
}

/// OCIO chroma_compress_fwd: scene M at scene J to the compressed M at the tonemapped J @p j_ts.
A2R_INLINE float A2rChromaCompressForward(float J, float M, float j_ts, float m_norm,
                                          float reach_max_m, A2R_GLOBAL const float* p) {
  if (M == 0.0f) return M;
  const float model_gamma_inv = p[ALCEDO_A2R_MODEL_GAMMA_INV];
  const float nJ              = j_ts / p[ALCEDO_A2R_LIMIT_J_MAX];
  const float snJ             = A2rMax(0.0f, 1.0f - nJ);
  const float limit           = A2R_POW(nJ, model_gamma_inv) * reach_max_m / m_norm;
  float       m_cp            = M * A2R_POW(j_ts / J, model_gamma_inv);
  m_cp                        = m_cp / m_norm;
  m_cp = limit - A2rToeForward(limit - m_cp, limit - 0.001f, snJ * p[ALCEDO_A2R_SAT],
                               A2R_SQRT(nJ * nJ + p[ALCEDO_A2R_SAT_THR]));
  m_cp = A2rToeForward(m_cp, limit, nJ * p[ALCEDO_A2R_COMPR], snJ);
  return m_cp * m_norm;
}

/// OCIO chroma_compress_inv: compressed M at the tonemapped J @p j_ts to the scene M at @p J.
A2R_INLINE float A2rChromaCompressInverse(float j_ts, float m_cp, float J, float m_norm,
                                          float reach_max_m, A2R_GLOBAL const float* p) {
  if (m_cp == 0.0f) return m_cp;
  const float model_gamma_inv = p[ALCEDO_A2R_MODEL_GAMMA_INV];
  const float nJ              = j_ts / p[ALCEDO_A2R_LIMIT_J_MAX];
  const float snJ             = A2rMax(0.0f, 1.0f - nJ);
  const float limit           = A2R_POW(nJ, model_gamma_inv) * reach_max_m / m_norm;
  float       M               = m_cp / m_norm;
  M                           = A2rToeInverse(M, limit, nJ * p[ALCEDO_A2R_COMPR], snJ);
  M = limit - A2rToeInverse(limit - M, limit - 0.001f, snJ * p[ALCEDO_A2R_SAT],
                            A2R_SQRT(nJ * nJ + p[ALCEDO_A2R_SAT_THR]));
  M = M * m_norm;
  return M * A2R_POW(j_ts / J, -model_gamma_inv);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

/**
 * ACES 2.0 output transform, forward: ACES2065-1 (AP0) scene-linear RGB to display-linear RGB
 * in the display primaries (1.0 = 100 nits). Matches OCIO's forward
 * FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20 followed by the limiting to display matrix, without a
 * clamp. A color whose achromatic response is not positive returns black (OCIO returns NaN for a
 * negative response).
 */
A2R_INLINE A2rFloat3 A2rAp0ToDisplay(A2rFloat3 ap0, A2R_GLOBAL const float* p) {
  const A2rFloat3 aab =
      A2rRgbToAab(ap0, p + ALCEDO_A2R_AP0_RGB_TO_CAM16, p + ALCEDO_A2R_AP0_CONE_TO_AAB);
  if (aab.x <= 0.0f) return A2rMake3(0.0f, 0.0f, 0.0f);
  const A2rFloat3 jmh         = A2rAabToJmh(aab, p);
  const float     h_rad       = jmh.z * (A2R_PI / 180.0f);
  const float     cos_hr      = A2R_COS(h_rad);
  const float     sin_hr      = A2R_SIN(h_rad);
  const float     reach_max_m = A2rReachMaxM(jmh.z, p);
  const float m_norm = A2rChromaCompressNorm(cos_hr, sin_hr, p[ALCEDO_A2R_CHROMA_COMPRESS_SCALE]);

  const float j_ts   = A2rTonescaleForwardFromA(aab.x, p);
  const float m_cp   = A2rChromaCompressForward(jmh.x, jmh.y, j_ts, m_norm, reach_max_m, p);
  A2rFloat3   compressed = A2rMake3(j_ts, m_cp, jmh.z);
  if (j_ts <= 0.0f) {
    compressed = A2rMake3(0.0f, 0.0f, jmh.z);
  } else {
    const A2rFloat3 cusp = A2rCuspSample(jmh.z, p);
    compressed           = A2rCompressGamut(compressed, j_ts, cusp, reach_max_m, 0, p);
  }
  const A2rFloat3 limit_rgb =
      A2rJmhToRgb(compressed, cos_hr, sin_hr, p + ALCEDO_A2R_LIMIT_AAB_TO_CONE,
                  p + ALCEDO_A2R_LIMIT_CAM16_TO_RGB, p);
  return A2rMul(p + ALCEDO_A2R_LIMIT_TO_DISPLAY, limit_rgb);
}

/**
 * ACES 2.0 output transform, inverse: display-linear RGB in the display primaries (1.0 = 100
 * nits) to ACES2065-1 (AP0) scene-linear RGB. The limiting RGB is clamped to [0, peak / 100]
 * first; the result is not clamped. Matches OCIO's inverse FIXED_FUNCTION_ACES_OUTPUT_TRANSFORM_20.
 */
A2R_INLINE A2rFloat3 A2rDisplayToAp0(A2rFloat3 display_rgb, A2R_GLOBAL const float* p) {
  const A2rFloat3 limit_rgb = A2rMul(p + ALCEDO_A2R_DISPLAY_TO_LIMIT, display_rgb);
  const float     input_max = p[ALCEDO_A2R_INPUT_MAX];
  const A2rFloat3 clamped =
      A2rMake3(A2rClamp(limit_rgb.x, 0.0f, input_max), A2rClamp(limit_rgb.y, 0.0f, input_max),
               A2rClamp(limit_rgb.z, 0.0f, input_max));
  const A2rFloat3 compressed_jmh = A2rAabToJmh(
      A2rRgbToAab(clamped, p + ALCEDO_A2R_LIMIT_RGB_TO_CAM16, p + ALCEDO_A2R_LIMIT_CONE_TO_AAB), p);
  if (compressed_jmh.x <= 0.0f) return A2rMake3(0.0f, 0.0f, 0.0f);

  const float h_rad       = compressed_jmh.z * (A2R_PI / 180.0f);
  const float cos_hr      = A2R_COS(h_rad);
  const float sin_hr      = A2R_SIN(h_rad);
  const float reach_max_m = A2rReachMaxM(compressed_jmh.z, p);
  const float m_norm   = A2rChromaCompressNorm(cos_hr, sin_hr, p[ALCEDO_A2R_CHROMA_COMPRESS_SCALE]);

  // Inverse gamut compression: closed form below the analytic threshold, one estimate of Jx
  // and a second evaluation above it.
  const A2rFloat3 cusp = A2rCuspSample(compressed_jmh.z, p);
  float           jx   = compressed_jmh.x;
  if (jx > A2rLerp(cusp.x, p[ALCEDO_A2R_LIMIT_J_MAX], 0.3f)) {
    jx = A2rCompressGamut(compressed_jmh, jx, cusp, reach_max_m, 1, p).x;
  }
  const A2rFloat3 tonemapped = A2rCompressGamut(compressed_jmh, jx, cusp, reach_max_m, 1, p);

  const float     J          = A2rTonescaleInverse(tonemapped.x, p);
  const float M = A2rChromaCompressInverse(tonemapped.x, tonemapped.y, J, m_norm, reach_max_m, p);
  return A2rJmhToRgb(A2rMake3(J, M, tonemapped.z), cos_hr, sin_hr, p + ALCEDO_A2R_AP0_AAB_TO_CONE,
                     p + ALCEDO_A2R_AP0_CAM16_TO_RGB, p);
}

#undef A2R_GLOBAL
#undef A2R_INLINE
#undef A2R_POW
#undef A2R_SQRT
#undef A2R_ATAN2
#undef A2R_COS
#undef A2R_SIN
#undef A2R_LOG10
#undef A2R_FABS
#undef A2R_FLOOR
#undef A2R_CAM_NL_OFFSET
#undef A2R_PI
#endif
