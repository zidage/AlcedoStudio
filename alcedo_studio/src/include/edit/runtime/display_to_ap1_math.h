// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Portions of this file are a port of OpenColorIO 2.5.1
// (src/OpenColorIO/ops/fixedfunction/ACES2/Transform.cpp and FixedFunctionOpGPU.cpp):
// Copyright Contributors to the OpenColorIO Project. SPDX-License-Identifier: BSD-3-Clause.
// See THIRD_PARTY_NOTICE.txt.
#ifndef ALCEDO_DISPLAY_TO_AP1_MATH_H
#define ALCEDO_DISPLAY_TO_AP1_MATH_H

// Raster input color conversion to the ACEScc AP1 working space, shared by the host reference,
// CUDA, OpenCL C and Metal (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, 5.3 and
// 5.5).
//
// Display-referred branch: the OpenColorIO 2.5.1 ACES 2.0 output transform inverse
// (Renderer_ACES_OutputTransform20::inv), with the closed-form gamut boundary of the OCIO GPU
// shader generator. Scene-linear branch: one 3x3 matrix and the ACES 1.3 reference gamut
// compression. Both write ACEScc-encoded AP1.
//
// All parameters and tables live in one float buffer with the layout below. The host fills it
// (aces2_inverse_runtime.cpp) and each backend uploads it as one device buffer.

// ACEScc comes from color/color_encoding_math.h. OpenCL programs list it before this file; Metal
// shaders include it first.
#if !defined(__OPENCL_VERSION__) && !defined(__OPENCL_C_VERSION__) && !defined(__METAL_VERSION__)
#include "color/color_encoding_math.h"
#endif

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define D2A_GLOBAL __global
#define D2A_INLINE static inline
#define D2A_POW    pow
#define D2A_SQRT   sqrt
#define D2A_ATAN2  atan2
#define D2A_COS    cos
#define D2A_SIN    sin
#define D2A_LOG10  log10
#define D2A_FABS   fabs
#define D2A_FLOOR  floor
#elif defined(__METAL_VERSION__)
#define D2A_GLOBAL device
#define D2A_INLINE static inline
#define D2A_POW    pow
#define D2A_SQRT   sqrt
#define D2A_ATAN2  atan2
#define D2A_COS    cos
#define D2A_SIN    sin
#define D2A_LOG10  log10
#define D2A_FABS   fabs
#define D2A_FLOOR  floor
#else
#include <cmath>
#define D2A_GLOBAL
#ifdef __CUDACC__
#define D2A_INLINE static __host__ __device__ inline
#else
#define D2A_INLINE static inline
#endif
#define D2A_POW   powf
#define D2A_SQRT  sqrtf
#define D2A_ATAN2 atan2f
#define D2A_COS   cosf
#define D2A_SIN   sinf
#define D2A_LOG10 log10f
#define D2A_FABS  fabsf
#define D2A_FLOOR floorf
#endif

// ---------------------------------------------------------------------------------------------
// Packed parameter layout (float indices)
// ---------------------------------------------------------------------------------------------

/// 0: display-referred ACES 2.0 inverse. 1: scene-linear matrix.
#define ALCEDO_D2A_BRANCH                 0
/// Display branch: source RGB to limiting RGB. Scene branch: source RGB to AP1.
#define ALCEDO_D2A_SOURCE_TO_TARGET       1
/// Floats that the scene-linear branch reads.
#define ALCEDO_D2A_SCENE_PACKED_SIZE      10
#define ALCEDO_D2A_LIMIT_RGB_TO_CAM16     10
#define ALCEDO_D2A_LIMIT_CONE_TO_AAB      19
#define ALCEDO_D2A_AP0_AAB_TO_CONE        28
#define ALCEDO_D2A_AP0_CAM16_TO_RGB       37
#define ALCEDO_D2A_AP0_TO_AP1             46
#define ALCEDO_D2A_SCALARS                55
#define ALCEDO_D2A_INPUT_MAX              (ALCEDO_D2A_SCALARS + 0)
#define ALCEDO_D2A_AP1_MAX                (ALCEDO_D2A_SCALARS + 1)
#define ALCEDO_D2A_CZ                     (ALCEDO_D2A_SCALARS + 2)
#define ALCEDO_D2A_INV_CZ                 (ALCEDO_D2A_SCALARS + 3)
#define ALCEDO_D2A_AP0_A_W_J              (ALCEDO_D2A_SCALARS + 4)
#define ALCEDO_D2A_AP0_INV_A_W_J          (ALCEDO_D2A_SCALARS + 5)
#define ALCEDO_D2A_AP0_F_L_N              (ALCEDO_D2A_SCALARS + 6)
#define ALCEDO_D2A_TS_INVERSE_LIMIT       (ALCEDO_D2A_SCALARS + 7)
#define ALCEDO_D2A_TS_T_1                 (ALCEDO_D2A_SCALARS + 8)
#define ALCEDO_D2A_TS_S_2                 (ALCEDO_D2A_SCALARS + 9)
#define ALCEDO_D2A_TS_M_2                 (ALCEDO_D2A_SCALARS + 10)
#define ALCEDO_D2A_TS_G                   (ALCEDO_D2A_SCALARS + 11)
#define ALCEDO_D2A_LIMIT_J_MAX            (ALCEDO_D2A_SCALARS + 12)
#define ALCEDO_D2A_MODEL_GAMMA_INV        (ALCEDO_D2A_SCALARS + 13)
#define ALCEDO_D2A_SAT                    (ALCEDO_D2A_SCALARS + 14)
#define ALCEDO_D2A_SAT_THR                (ALCEDO_D2A_SCALARS + 15)
#define ALCEDO_D2A_COMPR                  (ALCEDO_D2A_SCALARS + 16)
#define ALCEDO_D2A_CHROMA_COMPRESS_SCALE  (ALCEDO_D2A_SCALARS + 17)
#define ALCEDO_D2A_MID_J                  (ALCEDO_D2A_SCALARS + 18)
#define ALCEDO_D2A_FOCUS_DIST             (ALCEDO_D2A_SCALARS + 19)
#define ALCEDO_D2A_LOWER_HULL_GAMMA_INV   (ALCEDO_D2A_SCALARS + 20)
#define ALCEDO_D2A_HUE_SEARCH_LO          (ALCEDO_D2A_SCALARS + 21)
#define ALCEDO_D2A_HUE_SEARCH_HI          (ALCEDO_D2A_SCALARS + 22)
#define ALCEDO_D2A_SCALAR_COUNT           32
/// Hue tables: 360 nominal entries, 1 lower and 2 upper wrap entries (OCIO layout).
#define ALCEDO_D2A_TABLE_SIZE             363
#define ALCEDO_D2A_TABLE_BASE_INDEX       1
#define ALCEDO_D2A_TABLE_LOWER_WRAP_INDEX 0
#define ALCEDO_D2A_TABLE_UPPER_WRAP_INDEX 361
#define ALCEDO_D2A_REACH_TABLE            (ALCEDO_D2A_SCALARS + ALCEDO_D2A_SCALAR_COUNT)
#define ALCEDO_D2A_HUE_TABLE              (ALCEDO_D2A_REACH_TABLE + ALCEDO_D2A_TABLE_SIZE)
/// Interleaved cusp J, cusp M and upper hull gamma inverse per entry.
#define ALCEDO_D2A_CUSP_TABLE             (ALCEDO_D2A_HUE_TABLE + ALCEDO_D2A_TABLE_SIZE)
#define ALCEDO_D2A_PACKED_SIZE            (ALCEDO_D2A_CUSP_TABLE + 3 * ALCEDO_D2A_TABLE_SIZE)

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

typedef struct {
  float x, y, z;
} D2aFloat3;

D2A_INLINE D2aFloat3 D2aMake3(float x, float y, float z) {
  D2aFloat3 v;
  v.x = x;
  v.y = y;
  v.z = z;
  return v;
}

D2A_INLINE float     D2aMin(float a, float b) { return a < b ? a : b; }
D2A_INLINE float     D2aMax(float a, float b) { return a > b ? a : b; }
D2A_INLINE float     D2aClamp(float x, float lo, float hi) { return D2aMin(hi, D2aMax(lo, x)); }
D2A_INLINE float     D2aSign(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }
D2A_INLINE float     D2aLerp(float a, float b, float t) { return (b - a) * t + a; }

D2A_INLINE D2aFloat3 D2aMul(D2A_GLOBAL const float* m, D2aFloat3 v) {
  return D2aMake3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                  m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

// ACES 1.3 reference gamut compression, identical to aces_reference_gamut_compression.h.
D2A_INLINE float D2aRgcCompressDistance(float distance, float limit, float threshold) {
  const float power = 1.2f;
  if (distance < threshold) return distance;
  const float threshold_difference = limit - threshold;
  const float one_minus_threshold  = 1.0f - threshold;
  const float inner                = D2A_POW(one_minus_threshold / threshold_difference, -power);
  const float denominator          = D2A_POW(D2aMax(inner - 1.0f, 0.0f), 1.0f / power);
  if (denominator <= 1.0e-6f) return threshold;
  const float scale      = threshold_difference / denominator;
  const float normalized = (distance - threshold) / scale;
  const float shaped     = D2A_POW(D2aMax(normalized, 0.0f), power);
  return threshold + scale * normalized / D2A_POW(1.0f + shaped, 1.0f / power);
}

D2A_INLINE D2aFloat3 D2aReferenceGamutCompress(D2aFloat3 c) {
  const float achromatic = D2aMax(c.x, D2aMax(c.y, c.z));
  const float magnitude  = achromatic < 0.0f ? -achromatic : achromatic;
  if (magnitude <= 1.0e-6f) return c;
  const float cyan    = (achromatic - c.x) / magnitude;
  const float magenta = (achromatic - c.y) / magnitude;
  const float yellow  = (achromatic - c.z) / magnitude;
  return D2aMake3(achromatic - D2aRgcCompressDistance(cyan, 1.147f, 0.815f) * magnitude,
                  achromatic - D2aRgcCompressDistance(magenta, 1.264f, 0.803f) * magnitude,
                  achromatic - D2aRgcCompressDistance(yellow, 1.312f, 0.880f) * magnitude);
}

// ---------------------------------------------------------------------------------------------
// Hellwig CAM (OCIO ACES2 constants)
// ---------------------------------------------------------------------------------------------

#define D2A_CAM_NL_OFFSET 27.13f /* 0.2713 * 100 */
#define D2A_PI            3.14159265358979f

D2A_INLINE float D2aConeCompressFwd(float v) {
  const float f = D2A_POW(D2A_FABS(v), 0.42f);
  return D2aSign(v) * f / (D2A_CAM_NL_OFFSET + f);
}

D2A_INLINE float D2aConeCompressInv(float v) {
  const float lim = D2aMin(D2A_FABS(v), 0.99f);
  return D2aSign(v) * D2A_POW(D2A_CAM_NL_OFFSET * lim / (1.0f - lim), 1.0f / 0.42f);
}

/// Display RGB in the limiting primaries to JMh (h in degrees, [0, 360)).
D2A_INLINE D2aFloat3 D2aLimitRgbToJmh(D2aFloat3 rgb, D2A_GLOBAL const float* p) {
  const D2aFloat3 lms = D2aMul(p + ALCEDO_D2A_LIMIT_RGB_TO_CAM16, rgb);
  const D2aFloat3 rgb_a =
      D2aMake3(D2aConeCompressFwd(lms.x), D2aConeCompressFwd(lms.y), D2aConeCompressFwd(lms.z));
  const D2aFloat3 aab = D2aMul(p + ALCEDO_D2A_LIMIT_CONE_TO_AAB, rgb_a);
  if (aab.x <= 0.0f) return D2aMake3(0.0f, 0.0f, 0.0f);
  const float J = 100.0f * D2A_POW(aab.x, p[ALCEDO_D2A_CZ]);
  const float M = (J == 0.0f) ? 0.0f : D2A_SQRT(aab.y * aab.y + aab.z * aab.z);
  float       h = (aab.y == 0.0f) ? 0.0f : D2A_ATAN2(aab.z, aab.y) * (180.0f / D2A_PI);
  h             = h - D2A_FLOOR(h / 360.0f) * 360.0f;
  h             = (h < 0.0f) ? h + 360.0f : h;
  return D2aMake3(J, M, h);
}

/// JMh to ACES2065-1 (AP0) RGB.
D2A_INLINE D2aFloat3 D2aJmhToAp0(D2aFloat3 jmh, float cos_hr, float sin_hr,
                                 D2A_GLOBAL const float* p) {
  const D2aFloat3 aab =
      D2aMake3(D2A_POW(jmh.x * 0.01f, p[ALCEDO_D2A_INV_CZ]), jmh.y * cos_hr, jmh.y * sin_hr);
  const D2aFloat3 rgb_a = D2aMul(p + ALCEDO_D2A_AP0_AAB_TO_CONE, aab);
  const D2aFloat3 lms   = D2aMake3(D2aConeCompressInv(rgb_a.x), D2aConeCompressInv(rgb_a.y),
                                   D2aConeCompressInv(rgb_a.z));
  return D2aMul(p + ALCEDO_D2A_AP0_CAM16_TO_RGB, lms);
}

// ---------------------------------------------------------------------------------------------
// Table lookups
// ---------------------------------------------------------------------------------------------

D2A_INLINE float D2aReachMaxM(float h, D2A_GLOBAL const float* p) {
  const float i_base = D2A_FLOOR(h);
  const int   i_lo   = (int)i_base + ALCEDO_D2A_TABLE_BASE_INDEX;
  const float lo     = p[ALCEDO_D2A_REACH_TABLE + i_lo];
  const float hi     = p[ALCEDO_D2A_REACH_TABLE + i_lo + 1];
  return D2aLerp(lo, hi, h - i_base);
}

/// Cusp J, cusp M and upper hull gamma inverse at hue @p h.
D2A_INLINE D2aFloat3 D2aCuspSample(float h, D2A_GLOBAL const float* p) {
  D2A_GLOBAL const float* hues = p + ALCEDO_D2A_HUE_TABLE;
  int                     i    = (int)h + ALCEDO_D2A_TABLE_BASE_INDEX;
  int                     i_lo = i + (int)p[ALCEDO_D2A_HUE_SEARCH_LO];
  int                     i_hi = i + (int)p[ALCEDO_D2A_HUE_SEARCH_HI];
  if (i_lo < ALCEDO_D2A_TABLE_LOWER_WRAP_INDEX) i_lo = ALCEDO_D2A_TABLE_LOWER_WRAP_INDEX;
  if (i_hi > ALCEDO_D2A_TABLE_UPPER_WRAP_INDEX) i_hi = ALCEDO_D2A_TABLE_UPPER_WRAP_INDEX;
  while (i_lo + 1 < i_hi) {
    if (h > hues[i]) {
      i_lo = i;
    } else {
      i_hi = i;
    }
    i = (i_lo + i_hi) / 2;
  }
  if (i_hi < 1) i_hi = 1;
  D2A_GLOBAL const float* lo = p + ALCEDO_D2A_CUSP_TABLE + 3 * (i_hi - 1);
  D2A_GLOBAL const float* hi = p + ALCEDO_D2A_CUSP_TABLE + 3 * i_hi;
  const float             t  = (h - hues[i_hi - 1]) / (hues[i_hi] - hues[i_hi - 1]);
  return D2aMake3(D2aLerp(lo[0], hi[0], t), D2aLerp(lo[1], hi[1], t), D2aLerp(lo[2], hi[2], t));
}

// ---------------------------------------------------------------------------------------------
// Inverse gamut compression
// ---------------------------------------------------------------------------------------------

D2A_INLINE float D2aFocusGain(float J, float cusp_j, float limit_j_max) {
  const float thr = D2aLerp(cusp_j, limit_j_max, 0.3f);
  if (J > thr) {
    float gain = (limit_j_max - thr) / D2aMax(0.0001f, limit_j_max - J);
    gain       = D2A_LOG10(gain);
    return gain * gain + 1.0f;
  }
  return 1.0f;
}

D2A_INLINE float D2aSolveJIntersect(float J, float M, float focus_j, float limit_j_max,
                                    float slope_gain) {
  const float m_scaled = M / slope_gain;
  const float a        = m_scaled / focus_j;
  if (J < focus_j) {
    const float b = 1.0f - m_scaled;
    const float c = -J;
    return -2.0f * c / (b + D2A_SQRT(b * b - 4.0f * a * c));
  }
  const float b = -(1.0f + m_scaled + limit_j_max * a);
  const float c = limit_j_max * m_scaled + J;
  return -2.0f * c / (b - D2A_SQRT(b * b - 4.0f * a * c));
}

/// Closed-form gamut boundary of the OCIO GPU shader generator.
D2A_INLINE float D2aGamutBoundaryM(D2aFloat3 cusp, float gamma_bottom_inv, float j_source,
                                   float j_cusp, float slope, float limit_j_max) {
  const float lower =
      j_cusp * D2A_POW(j_source / j_cusp, gamma_bottom_inv) / (cusp.x / cusp.y - slope);
  const float upper = cusp.y * (limit_j_max - j_cusp) *
                      D2A_POW((limit_j_max - j_source) / (limit_j_max - j_cusp), cusp.z) /
                      (slope * cusp.y + limit_j_max - cusp.x);
  const float s = 0.12f * cusp.y;
  const float h = D2aMax(s - D2A_FABS(lower - upper), 0.0f) / s;
  return D2aMin(lower, upper) - h * h * h * s * (1.0f / 6.0f);
}

D2A_INLINE float D2aRemapMInverse(float M, float gamut_boundary_m, float reach_boundary_m) {
  const float proportion = D2aMax(gamut_boundary_m / reach_boundary_m, 0.75f);
  const float threshold  = proportion * gamut_boundary_m;
  if (proportion >= 1.0f || M <= threshold) return M;
  const float m_offset     = M - threshold;
  const float gamut_offset = gamut_boundary_m - threshold;
  const float reach_offset = reach_boundary_m - threshold;
  const float scale        = reach_offset / ((reach_offset / gamut_offset) - 1.0f);
  const float nd           = m_offset / scale;
  if (nd >= 1.0f) return threshold + scale;
  return threshold + scale * -(nd / (nd - 1.0f));
}

D2A_INLINE D2aFloat3 D2aCompressGamutInverse(D2aFloat3 jmh, float jx, D2aFloat3 cusp,
                                             float reach_max_m, D2A_GLOBAL const float* p) {
  const float limit_j_max = p[ALCEDO_D2A_LIMIT_J_MAX];
  if (jmh.y <= 0.0f || jmh.x > limit_j_max) return D2aMake3(jmh.x, 0.0f, jmh.z);
  const float focus_j =
      D2aLerp(cusp.x, p[ALCEDO_D2A_MID_J], D2aMin(1.0f, 1.3f - cusp.x / limit_j_max));
  const float slope_gain =
      limit_j_max * p[ALCEDO_D2A_FOCUS_DIST] * D2aFocusGain(jx, cusp.x, limit_j_max);
  const float j_source = D2aSolveJIntersect(jmh.x, jmh.y, focus_j, limit_j_max, slope_gain);
  float       slope    = (j_source < focus_j) ? j_source : (limit_j_max - j_source);
  slope                = slope * (j_source - focus_j) / (focus_j * slope_gain);
  const float j_cusp   = D2aSolveJIntersect(cusp.x, cusp.y, focus_j, limit_j_max, slope_gain);
  const float gamut_boundary_m = D2aGamutBoundaryM(cusp, p[ALCEDO_D2A_LOWER_HULL_GAMMA_INV],
                                                   j_source, j_cusp, slope, limit_j_max);
  if (gamut_boundary_m <= 0.0f) return D2aMake3(jmh.x, 0.0f, jmh.z);
  float reach_boundary_m =
      limit_j_max * D2A_POW(j_source / limit_j_max, p[ALCEDO_D2A_MODEL_GAMMA_INV]);
  reach_boundary_m       = reach_boundary_m / ((limit_j_max / reach_max_m) - slope);
  const float remapped_m = D2aRemapMInverse(jmh.y, gamut_boundary_m, reach_boundary_m);
  return D2aMake3(j_source + remapped_m * slope, remapped_m, jmh.z);
}

// ---------------------------------------------------------------------------------------------
// Inverse tonescale and chroma compression
// ---------------------------------------------------------------------------------------------

D2A_INLINE float D2aTonescaleInverse(float J, D2A_GLOBAL const float* p) {
  const float a_w_j = p[ALCEDO_D2A_AP0_A_W_J];
  const float f_l_n = p[ALCEDO_D2A_AP0_F_L_N];
  // J -> Y (nits): _J_to_Y.
  const float A     = D2A_POW(D2A_FABS(J) * 0.01f, p[ALCEDO_D2A_INV_CZ]);
  const float y_in  = D2aConeCompressInv(a_w_j * A) / f_l_n;
  // Closed-form inverse tonescale: aces_tonescale<inverse>.
  const float Z     = D2aMax(0.0f, D2aMin(p[ALCEDO_D2A_TS_INVERSE_LIMIT], y_in * 0.01f));
  const float f     = (Z + D2A_SQRT(Z * (4.0f * p[ALCEDO_D2A_TS_T_1] + Z))) * 0.5f;
  const float Y =
      p[ALCEDO_D2A_TS_S_2] / (D2A_POW(p[ALCEDO_D2A_TS_M_2] / f, 1.0f / p[ALCEDO_D2A_TS_G]) - 1.0f);
  // Y -> J: _Y_to_J.
  const float ra    = D2aConeCompressFwd(D2A_FABS(Y) * f_l_n);
  const float j_out = 100.0f * D2A_POW(ra * p[ALCEDO_D2A_AP0_INV_A_W_J], p[ALCEDO_D2A_CZ]);
  return D2aSign(J) * j_out;
}

D2A_INLINE float D2aToeInverse(float x, float limit, float k1_in, float k2_in) {
  if (x > limit) return x;
  const float k2 = D2aMax(k2_in, 0.001f);
  const float k1 = D2A_SQRT(k1_in * k1_in + k2 * k2);
  const float k3 = (limit + k1) / (limit + k2);
  return (x * x + k1 * x) / (k3 * (x + k2));
}

D2A_INLINE float D2aChromaCompressNorm(float cos_hr, float sin_hr, float scale) {
  const float cos_hr2 = 2.0f * cos_hr * cos_hr - 1.0f;
  const float sin_hr2 = 2.0f * cos_hr * sin_hr;
  const float cos_hr3 = 4.0f * cos_hr * cos_hr * cos_hr - 3.0f * cos_hr;
  const float sin_hr3 = 3.0f * sin_hr - 4.0f * sin_hr * sin_hr * sin_hr;
  const float M       = 11.34072f * cos_hr + 16.46899f * cos_hr2 + 7.88380f * cos_hr3 +
                  14.66441f * sin_hr + -6.37224f * sin_hr2 + 9.19364f * sin_hr3 + 77.12896f;
  return M * scale;
}

D2A_INLINE float D2aChromaCompressInverse(float j_ts, float m_cp, float J, float m_norm,
                                          float reach_max_m, D2A_GLOBAL const float* p) {
  if (m_cp == 0.0f) return m_cp;
  const float model_gamma_inv = p[ALCEDO_D2A_MODEL_GAMMA_INV];
  const float nJ              = j_ts / p[ALCEDO_D2A_LIMIT_J_MAX];
  const float snJ             = D2aMax(0.0f, 1.0f - nJ);
  const float limit           = D2A_POW(nJ, model_gamma_inv) * reach_max_m / m_norm;
  float       M               = m_cp / m_norm;
  M                           = D2aToeInverse(M, limit, nJ * p[ALCEDO_D2A_COMPR], snJ);
  M = limit - D2aToeInverse(limit - M, limit - 0.001f, snJ * p[ALCEDO_D2A_SAT],
                            D2A_SQRT(nJ * nJ + p[ALCEDO_D2A_SAT_THR]));
  M = M * m_norm;
  return M * D2A_POW(j_ts / J, -model_gamma_inv);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

/**
 * Display-referred branch, steps 1 to 6 of plan section 5.3: display-linear source RGB
 * (1.0 = 100 nits) to ACES2065-1 scene-linear AP0, without the AP1 range clamp.
 */
D2A_INLINE D2aFloat3 D2aDisplayToAp0(D2aFloat3 source_rgb, D2A_GLOBAL const float* p) {
  const D2aFloat3 limit_rgb = D2aMul(p + ALCEDO_D2A_SOURCE_TO_TARGET, source_rgb);
  const float     input_max = p[ALCEDO_D2A_INPUT_MAX];
  const D2aFloat3 clamped =
      D2aMake3(D2aClamp(limit_rgb.x, 0.0f, input_max), D2aClamp(limit_rgb.y, 0.0f, input_max),
               D2aClamp(limit_rgb.z, 0.0f, input_max));
  const D2aFloat3 compressed_jmh = D2aLimitRgbToJmh(clamped, p);
  if (compressed_jmh.x <= 0.0f) return D2aMake3(0.0f, 0.0f, 0.0f);

  const float h_rad       = compressed_jmh.z * (D2A_PI / 180.0f);
  const float cos_hr      = D2A_COS(h_rad);
  const float sin_hr      = D2A_SIN(h_rad);
  const float reach_max_m = D2aReachMaxM(compressed_jmh.z, p);
  const float m_norm   = D2aChromaCompressNorm(cos_hr, sin_hr, p[ALCEDO_D2A_CHROMA_COMPRESS_SCALE]);

  // Inverse gamut compression: closed form below the analytic threshold, one estimate of Jx
  // and a second evaluation above it.
  const D2aFloat3 cusp = D2aCuspSample(compressed_jmh.z, p);
  float           jx   = compressed_jmh.x;
  if (jx > D2aLerp(cusp.x, p[ALCEDO_D2A_LIMIT_J_MAX], 0.3f)) {
    jx = D2aCompressGamutInverse(compressed_jmh, jx, cusp, reach_max_m, p).x;
  }
  const D2aFloat3 tonemapped = D2aCompressGamutInverse(compressed_jmh, jx, cusp, reach_max_m, p);

  const float     J          = D2aTonescaleInverse(tonemapped.x, p);
  const float M = D2aChromaCompressInverse(tonemapped.x, tonemapped.y, J, m_norm, reach_max_m, p);
  return D2aJmhToAp0(D2aMake3(J, M, tonemapped.z), cos_hr, sin_hr, p);
}

/**
 * Convert one source pixel to ACEScc-encoded AP1 (the develop_output encoding).
 *
 * Display-referred input goes through the ACES 2.0 inverse, AP0 to AP1 and the clamp to
 * [0, forward limit]. Scene-linear input goes through the source-to-AP1 matrix and the ACES 1.3
 * reference gamut compression.
 */
D2A_INLINE D2aFloat3 D2aSourceToAcesccAp1(D2aFloat3 source_rgb, D2A_GLOBAL const float* p) {
  D2aFloat3 ap1;
  if (p[ALCEDO_D2A_BRANCH] != 0.0f) {
    ap1 = D2aReferenceGamutCompress(D2aMul(p + ALCEDO_D2A_SOURCE_TO_TARGET, source_rgb));
  } else {
    const D2aFloat3 ap0     = D2aDisplayToAp0(source_rgb, p);
    const D2aFloat3 linear  = D2aMul(p + ALCEDO_D2A_AP0_TO_AP1, ap0);
    const float     ap1_max = p[ALCEDO_D2A_AP1_MAX];
    ap1 = D2aMake3(D2aClamp(linear.x, 0.0f, ap1_max), D2aClamp(linear.y, 0.0f, ap1_max),
                   D2aClamp(linear.z, 0.0f, ap1_max));
  }
  return D2aMake3(CeAcesccEncode(ap1.x), CeAcesccEncode(ap1.y), CeAcesccEncode(ap1.z));
}

#undef D2A_GLOBAL
#undef D2A_INLINE
#undef D2A_POW
#undef D2A_SQRT
#undef D2A_ATAN2
#undef D2A_COS
#undef D2A_SIN
#undef D2A_LOG10
#undef D2A_FABS
#undef D2A_FLOOR
#undef D2A_CAM_NL_OFFSET
#undef D2A_PI
#endif
