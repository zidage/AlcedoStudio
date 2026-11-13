// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_PROFILE_TONE_GPU_MATH_H
#define ALCEDO_PROFILE_TONE_GPU_MATH_H

// The profile tone stage of one pixel (dcp_profile_tone_curve_study.md, section 6.1), shared by the
// host, CUDA, OpenCL C, and Metal. The display transform kernels of the three backends run it as
// their first operation; the host packs the buffer (PackProfileToneGpuData) and keeps the scalar
// reference ApplyProfileToneStageReference for the tests.
#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define PROFILE_TONE_GLOBAL __global
#define PROFILE_TONE_INLINE static inline
#define PROFILE_TONE_POW    pow
#elif defined(__METAL_VERSION__)
#define PROFILE_TONE_GLOBAL device
#define PROFILE_TONE_INLINE static inline
#define PROFILE_TONE_POW    pow
#else
#include <cmath>
#define PROFILE_TONE_GLOBAL
#ifdef __CUDACC__
#define PROFILE_TONE_INLINE static __host__ __device__ inline
#else
#define PROFILE_TONE_INLINE static inline
#endif
#define PROFILE_TONE_POW powf
#endif

// Float buffer layout. Both tables have 4097 entries: one sample at each end of 4096 intervals.
#define PROFILE_TONE_TABLE_INTERVALS  4096
#define PROFILE_TONE_AP1_TO_PROPHOTO  0     // 9 floats, row-major, column vectors: `M`.
#define PROFILE_TONE_PROPHOTO_TO_AP1  9     // 9 floats: `M^-1`.
#define PROFILE_TONE_ANCHOR_SCALE     18    // 2^-E_a.
#define PROFILE_TONE_INVERSE_EXPONENT 19    // 1 / 2.4, the coordinate exponent of `N_m^-1`.
#define PROFILE_TONE_CURVE_TABLE      20    // `C` at j / 4096.
#define PROFILE_TONE_INVERSE_TABLE    4117  // `N_m^-1` at display light (i / 4096)^2.4.
#define PROFILE_TONE_GPU_FLOATS       8214

// Knee `k` of the highlight shoulder `H` at the curve input, ACR scale (raw white 1): one stop
// below raw white. Equal to kProfileToneHighlightKnee (profile_tone_reference.hpp).
#define PROFILE_TONE_HIGHLIGHT_KNEE   0.5f

typedef struct {
  float r, g, b;
} ProfileToneRgb;

PROFILE_TONE_INLINE ProfileToneRgb ProfileToneMakeRgb(float r, float g, float b) {
  ProfileToneRgb c;
  c.r = r;
  c.g = g;
  c.b = b;
  return c;
}

PROFILE_TONE_INLINE float ProfileToneClamp01(float x) {
  return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/// The highlight shoulder `H` at the curve input (study section 6.6): 0 below 0, identity up to the
/// knee `k`, then `1 - (1 - k)^2 / (x + 1 - 2k)`, which has slope 1 at the knee and stays below 1.
/// With `k` = 0.5, each stop of scene light above the knee halves the distance to the curve's
/// white, so scene light above raw white keeps its gradation.
PROFILE_TONE_INLINE float ProfileToneHighlight(float x) {
  const float k = PROFILE_TONE_HIGHLIGHT_KNEE;
  if (x <= k) return x < 0.0f ? 0.0f : x;
  return 1.0f - (1.0f - k) * (1.0f - k) / (x + 1.0f - 2.0f * k);
}

PROFILE_TONE_INLINE ProfileToneRgb ProfileToneMatrix(ProfileToneRgb                   c,
                                                     PROFILE_TONE_GLOBAL const float* m) {
  return ProfileToneMakeRgb(m[0] * c.r + m[1] * c.g + m[2] * c.b,
                            m[3] * c.r + m[4] * c.g + m[5] * c.b,
                            m[6] * c.r + m[7] * c.g + m[8] * c.b);
}

/// Linear interpolation in a 4097-entry table at @p coordinate on [0, 4096]; the last entry holds
/// at and above 4096 (`dng_1d_table::Interpolate`, DngToneCurve::Interpolate).
PROFILE_TONE_INLINE float ProfileToneTable(PROFILE_TONE_GLOBAL const float* table,
                                           float                            coordinate) {
  const unsigned index = (unsigned)coordinate;
  if (index >= (unsigned)PROFILE_TONE_TABLE_INTERVALS) return table[PROFILE_TONE_TABLE_INTERVALS];
  const float fract = coordinate - (float)index;
  return table[index] * (1.0f - fract) + table[index + 1] * fract;
}

/// `C` at scene value @p x, clamped to [0, 1].
PROFILE_TONE_INLINE float ProfileToneCurve(PROFILE_TONE_GLOBAL const float* data, float x) {
  return ProfileToneTable(data + PROFILE_TONE_CURVE_TABLE,
                          ProfileToneClamp01(x) * (float)PROFILE_TONE_TABLE_INTERVALS);
}

/// `N_m^-1` at display light @p display, clamped to [0, 1].
PROFILE_TONE_INLINE float ProfileToneInverse(PROFILE_TONE_GLOBAL const float* data, float display) {
  const float coordinate =
      PROFILE_TONE_POW(ProfileToneClamp01(display), data[PROFILE_TONE_INVERSE_EXPONENT]) *
      (float)PROFILE_TONE_TABLE_INTERVALS;
  return ProfileToneTable(data + PROFILE_TONE_INVERSE_TABLE, coordinate);
}

/// The DNG SDK baseline RGB tone (`RefBaselineRGBTone`, ApplyDngBaselineRgbTone): `C` on the
/// largest and the smallest channel; the middle channel keeps its relative position between them.
PROFILE_TONE_INLINE ProfileToneRgb ProfileToneBaselineRgb(ProfileToneRgb                   c,
                                                          PROFILE_TONE_GLOBAL const float* data) {
  const float r = c.r;
  const float g = c.g;
  const float b = c.b;
  float       hi;
  float       mid;
  float       lo;
  // Order: 0 = r >= g > b, 1 = b > r >= g, 2 = r >= b > g, 3 = r >= g == b, 4 = g > r >= b,
  // 5 = b > g > r, 6 = g >= b > r. The branches are those of RefBaselineRGBTone.
  int         order;
  if (r >= g) {
    if (g > b) {
      order = 0;
    } else if (b > r) {
      order = 1;
    } else if (b > g) {
      order = 2;
    } else {
      order = 3;
    }
  } else {
    if (r >= b) {
      order = 4;
    } else if (b > g) {
      order = 5;
    } else {
      order = 6;
    }
  }
  if (order == 3) {
    const float rr = ProfileToneCurve(data, r);
    const float gg = ProfileToneCurve(data, g);
    return ProfileToneMakeRgb(rr, gg, gg);
  }
  if (order == 0) {
    hi  = r;
    mid = g;
    lo  = b;
  } else if (order == 1) {
    hi  = b;
    mid = r;
    lo  = g;
  } else if (order == 2) {
    hi  = r;
    mid = b;
    lo  = g;
  } else if (order == 4) {
    hi  = g;
    mid = r;
    lo  = b;
  } else if (order == 5) {
    hi  = b;
    mid = g;
    lo  = r;
  } else {
    hi  = g;
    mid = b;
    lo  = r;
  }
  const float out_hi  = ProfileToneCurve(data, hi);
  const float out_lo  = ProfileToneCurve(data, lo);
  const float out_mid = out_lo + ((out_hi - out_lo) * (mid - lo) / (hi - lo));
  if (order == 0) return ProfileToneMakeRgb(out_hi, out_mid, out_lo);
  if (order == 1) return ProfileToneMakeRgb(out_mid, out_lo, out_hi);
  if (order == 2) return ProfileToneMakeRgb(out_hi, out_lo, out_mid);
  if (order == 4) return ProfileToneMakeRgb(out_mid, out_hi, out_lo);
  if (order == 5) return ProfileToneMakeRgb(out_lo, out_mid, out_hi);
  return ProfileToneMakeRgb(out_lo, out_hi, out_mid);
}

/// The stage on one pixel of linear AP1: `p = H(M * x * 2^-E_a)` per channel in linear ProPhoto,
/// `d = R_C(p)`, `s = N_m^-1(d)` per channel, and `M^-1 * s` in linear AP1.
PROFILE_TONE_INLINE ProfileToneRgb ProfileToneApplyStage(ProfileToneRgb                   ap1,
                                                         PROFILE_TONE_GLOBAL const float* data) {
  const float    anchor = data[PROFILE_TONE_ANCHOR_SCALE];
  ProfileToneRgb p      = ProfileToneMatrix(ap1, data + PROFILE_TONE_AP1_TO_PROPHOTO);
  p.r                   = ProfileToneHighlight(p.r * anchor);
  p.g                   = ProfileToneHighlight(p.g * anchor);
  p.b                   = ProfileToneHighlight(p.b * anchor);
  p                     = ProfileToneBaselineRgb(p, data);
  p.r                   = ProfileToneInverse(data, p.r);
  p.g                   = ProfileToneInverse(data, p.g);
  p.b                   = ProfileToneInverse(data, p.b);
  return ProfileToneMatrix(p, data + PROFILE_TONE_PROPHOTO_TO_AP1);
}

#endif  // ALCEDO_PROFILE_TONE_GPU_MATH_H
