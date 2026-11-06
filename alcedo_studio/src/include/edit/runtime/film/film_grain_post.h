// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_GRAIN_POST_H
#define ALCEDO_FILM_GRAIN_POST_H

// The DRT/Post Film Grain of ACES 2.0 and OpenDRT: the step that applies the mixed unit grain of
// film_grain_field.h to one display pixel (plan 'film_grain_format_and_stock_plan.md', section 6.3,
// row "apply post path"). Shared by the host tests and the backend kernels.
//
// The grain is a change of the density of the dye clouds, so it scales the linear display light:
//   y' = y * exp(-e_j),  e_j = strength * pixel_factor_j * tone_scale_j * T_j(q_j) * N_j
// - y is the linear display light of channel j, decoded with the transfer function of the output.
// - pixel_factor_j is the pixel integral of the plan (rule 'grain_pixel_integral'): the size of the
//   grain on the film at the pitch of the render.
// - T_j(q) = sqrt(q (1 - q)) * sigma_48_j(D_j(q)) is the tone form of the dye clouds of #660, with
//   the granularity curve of the stock at the gross density D_j(q) = lerp(low_j, high_j, q), and q
//   the sRGB code value of y clamped to [0, 1]. The grain is largest in the mid tones and goes to 0
//   at black and at the reference white, as on a print.
// - tone_scale_j sets the relative standard deviation of the linear display light at the 48 um
//   aperture to the post target at a neutral mid grey (film_grain_plan.hpp,
//   `MakeFilmGrainPostParams`).
// - N_j is the mixed unit grain. The colour of the grain is the colour of the 5207 scans of #660:
//   the mix gives the channel correlation, and the target gives r : g : b = 1.2 : 1 : 1.4.
// Near white the half of the grain that brightens gets smaller, so the grain does not push a
// highlight past white; the grain keeps its colour.
//
// OpenCL C cannot resolve #include, so an OpenCL program lists color_encoding_math.h and
// film_grain_field.h before this file. Metal shaders include it by relative path.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_GRAIN_POST_INLINE static inline
#define FILM_GRAIN_POST_EXP    exp
#define FILM_GRAIN_POST_SQRT   sqrt
#elif defined(__METAL_VERSION__)
#define FILM_GRAIN_POST_INLINE static inline
#define FILM_GRAIN_POST_EXP    exp
#define FILM_GRAIN_POST_SQRT   sqrt
#else
#include <cmath>

#include "color/color_encoding_math.h"
#include "edit/runtime/film/film_grain_field.h"
#ifdef __CUDACC__
#define FILM_GRAIN_POST_INLINE static __host__ __device__ inline
#else
#define FILM_GRAIN_POST_INLINE static inline
#endif
#define FILM_GRAIN_POST_EXP  expf
#define FILM_GRAIN_POST_SQRT sqrtf
#endif

/**
 * The apply step of one render. Copied by value into the kernels.
 *
 * `transfer` is the CE_TF_* id of the display encoding. `decode_scale` converts the decoded light
 * to the light relative to the reference white: CE_PQ_PEAK_NITS / reference white nits for ST
 * 2084, else 1. `low` and `high` are the first and the last node of each granularity curve.
 */
typedef struct {
  float          strength;
  int            transfer;
  float          decode_scale;
  float          pixel_factor[3];
  float          tone_scale[3];
  float          low[3];
  float          high[3];
  FilmGrainCurve curves[3];
} FilmGrainPostParams;

typedef struct {
  float r;
  float g;
  float b;
} FilmGrainPostRgb;

FILM_GRAIN_POST_INLINE float FilmGrainPostClamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/// T(q) of the header comment, for the curve @p curve and its node range [@p low, @p high].
FILM_GRAIN_POST_INLINE float FilmGrainPostTone(FilmGrainCurve curve, float low, float high,
                                               float q) {
  const float density = low + (high - low) * q;
  return FILM_GRAIN_POST_SQRT(q * (1.0f - q)) * FilmGrainCurveSigma(curve, density);
}

/// The tone coordinate q of the linear display light @p relative (1 = reference white).
FILM_GRAIN_POST_INLINE float FilmGrainPostToneCoordinate(float relative) {
  // The sRGB encode of 1 is 1 - 2^-24 in single precision, so light at or above the reference
  // white takes q = 1 here, and its grain is exactly 0.
  return relative >= 1.0f ? 1.0f : CeSrgbEncode(FilmGrainPostClamp01(relative));
}

FILM_GRAIN_POST_INLINE float FilmGrainPostSmoothstep(float edge0, float edge1, float value) {
  const float t = FilmGrainPostClamp01((value - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

/// One channel: the code value @p code with the grain @p unit (the mixed unit grain N_j).
FILM_GRAIN_POST_INLINE float FilmGrainPostChannel(FilmGrainPostParams params, int channel,
                                                  float code, float q, float unit, float brighten) {
  const float light = CeDecode(params.transfer, code);
  if (!(light > 0.0f)) {
    return code;
  }
  float error =
      params.strength * params.pixel_factor[channel] * params.tone_scale[channel] *
      FilmGrainPostTone(params.curves[channel], params.low[channel], params.high[channel], q) *
      unit;
  if (error == 0.0f) {
    return code;
  }
  error = error < 0.0f ? error * brighten : error;
  return CeEncode(params.transfer, light * FILM_GRAIN_POST_EXP(-error));
}

/**
 * Applies the grain @p unit (N of rule 'grain_layer_mix') to the display code values @p code.
 * A channel with no light (a code value at or below black) does not change.
 */
FILM_GRAIN_POST_INLINE FilmGrainPostRgb FilmGrainPostPixel(FilmGrainPostParams params,
                                                           FilmGrainPostRgb    code,
                                                           FilmGrainNoise3     unit) {
  const float qr =
      FilmGrainPostToneCoordinate(CeDecode(params.transfer, code.r) * params.decode_scale);
  const float qg =
      FilmGrainPostToneCoordinate(CeDecode(params.transfer, code.g) * params.decode_scale);
  const float qb =
      FilmGrainPostToneCoordinate(CeDecode(params.transfer, code.b) * params.decode_scale);
  // Near white, a negative density error (a brighter cloud) gets smaller.
  const float highlight = FilmGrainPostSmoothstep(0.72f, 0.96f, (qr + qg + qb) * (1.0f / 3.0f));
  const float brighten  = 1.0f + (0.12f - 1.0f) * highlight;
  FilmGrainPostRgb result;
  result.r = FilmGrainPostChannel(params, 0, code.r, qr, unit.x, brighten);
  result.g = FilmGrainPostChannel(params, 1, code.g, qg, unit.y, brighten);
  result.b = FilmGrainPostChannel(params, 2, code.b, qb, unit.z, brighten);
  return result;
}

#endif  // ALCEDO_FILM_GRAIN_POST_H
