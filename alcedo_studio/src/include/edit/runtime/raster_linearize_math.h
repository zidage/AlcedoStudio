// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_RASTER_LINEARIZE_MATH_H
#define ALCEDO_RASTER_LINEARIZE_MATH_H

// Raster input linearization (LinearizeRaster, raster_image_input_plan.md section 6.2), shared by
// the host reference, CUDA, OpenCL C and Metal. Code values become display-linear light in the
// source primaries with 1.0 = 100 nits; scene-linear input passes through.
//
// The host packs the parameters into one float buffer (raster_linearize_params.cpp):
//   header (ALCEDO_RL_HEADER_SIZE floats), then the 4096-entry sampled curves it references.

// The curves come from color/color_encoding_math.h. OpenCL programs list it before this file;
// Metal shaders include it first.
#if !defined(__OPENCL_VERSION__) && !defined(__OPENCL_C_VERSION__) && !defined(__METAL_VERSION__)
#include "color/color_encoding_math.h"
#endif

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define RL_GLOBAL __global
#define RL_INLINE static inline
#define RL_POW    pow
#define RL_FLOOR  floor
#elif defined(__METAL_VERSION__)
#define RL_GLOBAL device
#define RL_INLINE static inline
#define RL_POW    pow
#define RL_FLOOR  floor
#else
#include <cmath>
#define RL_GLOBAL
#ifdef __CUDACC__
#define RL_INLINE static __host__ __device__ inline
#else
#define RL_INLINE static inline
#endif
#define RL_POW   powf
#define RL_FLOOR floorf
#endif

/// 0: display-referred (apply the transfer). 1: scene-linear (pass through).
#define ALCEDO_RL_SCENE_LINEAR    0
/// 1 when all channels are HLG: the BT.2100 luminance OOTF of the 1000-nit display.
#define ALCEDO_RL_HLG_LUMINANCE   1
/// Factor from the curve's own linear range to 1.0 = 100 nits (100 for PQ, 10 for HLG).
#define ALCEDO_RL_OUTPUT_SCALE    2
/// Factor from stored code values to [0, 1] (1/255, 1/65535 or 1).
#define ALCEDO_RL_INPUT_SCALE     3
#define ALCEDO_RL_CHANNELS        4
/// Per channel: kind, gamma, ICC function type, ICC params g a b c d e f, sampled offset.
#define ALCEDO_RL_CHANNEL_STRIDE  11
#define ALCEDO_RL_HEADER_SIZE     (ALCEDO_RL_CHANNELS + 3 * ALCEDO_RL_CHANNEL_STRIDE)
#define ALCEDO_RL_SAMPLED_ENTRIES 4096

// Transfer kinds, equal to RasterTransferKind.
#define ALCEDO_RL_LINEAR          0
#define ALCEDO_RL_SRGB            1
#define ALCEDO_RL_GAMMA           2
#define ALCEDO_RL_BT1886          3
#define ALCEDO_RL_ICC_PARAMETRIC  4
#define ALCEDO_RL_ICC_SAMPLED     5
#define ALCEDO_RL_ST2084          6
#define ALCEDO_RL_HLG             7

typedef struct {
  float r, g, b;
} RlRgb;

RL_INLINE RlRgb RlMakeRgb(float r, float g, float b) {
  RlRgb c;
  c.r = r;
  c.g = g;
  c.b = b;
  return c;
}

RL_INLINE float RlMax(float a, float b) { return a > b ? a : b; }
RL_INLINE float RlMin(float a, float b) { return a < b ? a : b; }
RL_INLINE float RlPow0(float base, float exponent) {
  return base <= 0.0f ? 0.0f : RL_POW(base, exponent);
}

/// HLG inverse OETF of a signal clamped to [0, 1].
RL_INLINE float RlHlgInverseOetf(float v) { return CeHlgDecode(RlMin(v, 1.0f)); }

RL_INLINE float RlIccParametric(float x, RL_GLOBAL const float* c) {
  const float g = c[3], a = c[4], b = c[5], cc = c[6], d = c[7], e = c[8], f = c[9];
  switch ((int)c[2]) {
    case 0:
      return RlPow0(x, g);
    case 1:
      return (a != 0.0f && x >= -b / a) ? RlPow0(a * x + b, g) : 0.0f;
    case 2:
      return (a != 0.0f && x >= -b / a) ? RlPow0(a * x + b, g) + cc : cc;
    case 3:
      return x >= d ? RlPow0(a * x + b, g) : cc * x;
    case 4:
      return x >= d ? RlPow0(a * x + b, g) + e : cc * x + f;
    default:
      return x;
  }
}

RL_INLINE float RlSampled(float x, RL_GLOBAL const float* p, RL_GLOBAL const float* c) {
  RL_GLOBAL const float* table = p + (int)c[10];
  const float position = RlMin(RlMax(x, 0.0f), 1.0f) * (float)(ALCEDO_RL_SAMPLED_ENTRIES - 1);
  const int   lower    = (int)RL_FLOOR(position);
  const int   upper    = lower + 1 < ALCEDO_RL_SAMPLED_ENTRIES ? lower + 1 : lower;
  const float t        = position - (float)lower;
  return table[lower] * (1.0f - t) + table[upper] * t;
}

/// Linear light of one channel, relative to the curve's own maximum.
RL_INLINE float RlEvaluateChannel(float x, RL_GLOBAL const float* p, int channel) {
  RL_GLOBAL const float* c = p + ALCEDO_RL_CHANNELS + channel * ALCEDO_RL_CHANNEL_STRIDE;
  switch ((int)c[0]) {
    case ALCEDO_RL_LINEAR:
      return x;
    case ALCEDO_RL_SRGB:
      return CeSrgbDecode(x);
    case ALCEDO_RL_GAMMA:
      return CeGammaDecode(x, c[1]);
    case ALCEDO_RL_BT1886:
      return CeGammaDecode(x, CE_BT1886_GAMMA);
    case ALCEDO_RL_ICC_PARAMETRIC:
      return RlIccParametric(x, c);
    case ALCEDO_RL_ICC_SAMPLED:
      return RlSampled(x, p, c);
    case ALCEDO_RL_ST2084:
      return CePqDecode(RlMin(x, 1.0f));
    case ALCEDO_RL_HLG: {
      // Gray-axis form; the luminance form is applied in RlLinearize when all channels are HLG.
      return CeGammaDecode(RlHlgInverseOetf(x), CE_HLG_OOTF_GAMMA);
    }
    default:
      return x;
  }
}

/**
 * Linearize one pixel. @p r, @p g, @p b are stored code values (integer codes as floats, or
 * float samples); the result is display-linear (1.0 = 100 nits) or scene-linear source RGB.
 */
RL_INLINE RlRgb RlLinearize(float r, float g, float b, RL_GLOBAL const float* p) {
  const float in_scale = p[ALCEDO_RL_INPUT_SCALE];
  r *= in_scale;
  g *= in_scale;
  b *= in_scale;
  if (p[ALCEDO_RL_SCENE_LINEAR] != 0.0f) {
    return RlMakeRgb(r, g, b);
  }
  const float out_scale = p[ALCEDO_RL_OUTPUT_SCALE];
  if (p[ALCEDO_RL_HLG_LUMINANCE] != 0.0f) {
    const float sr = RlHlgInverseOetf(r), sg = RlHlgInverseOetf(g), sb = RlHlgInverseOetf(b);
    const float ys = CE_BT2100_LUMA_R * sr + CE_BT2100_LUMA_G * sg + CE_BT2100_LUMA_B * sb;
    const float k  = CeHlgOotfGain(ys);
    return RlMakeRgb(sr * k * out_scale, sg * k * out_scale, sb * k * out_scale);
  }
  // Display-referred code values below 0 have no light.
  return RlMakeRgb(RlEvaluateChannel(RlMax(r, 0.0f), p, 0) * out_scale,
                   RlEvaluateChannel(RlMax(g, 0.0f), p, 1) * out_scale,
                   RlEvaluateChannel(RlMax(b, 0.0f), p, 2) * out_scale);
}

#undef RL_GLOBAL
#undef RL_INLINE
#undef RL_POW
#undef RL_FLOOR
#endif
