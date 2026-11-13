// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_GRAIN_NEGATIVE_H
#define ALCEDO_FILM_GRAIN_NEGATIVE_H

// The grain in the negative of the film print: the step that adds the mixed unit grain of
// film_grain_field.h to the dye amounts of one pixel (plan 'film_grain_format_and_stock_plan.md',
// section 6.3, row "apply film print"; phase G5). Shared by the host tests and the backend kernels.
//
// The grain is a change of the dye amount of each layer:
//   d'_k = d_k + scale_k * sigma_k(d_k) * N_k,  scale_k = strength * pixel_factor_k
// - d_k is the dye amount of layer k after the inhibitor step (FP5).
// - sigma_k is the layer curve of the stock: the standard deviation of the dye amount at the 48 um
//   aperture against the dye amount (bundle `grain/<film>/layer_*`).
// - pixel_factor_k converts the 48 um value to the pitch of the render (rule
//   'grain_pixel_integral').
// - N is the mixed unit grain, with the layer correlation R of the 5207 scans (rule
//   'grain_layer_mix'). The dye spectra and the print or scanner table give the grain its colour.
// A colour reversal film stores the removed dye `record`, and its dye amount is
// `dye_max - record` (lab rule 'removed_dye_record'). The step then reads the curve at that amount
// and subtracts the change from the record, so the dye amount gets the same change as on a
// negative.
//
// OpenCL C cannot resolve #include, so an OpenCL program lists film_grain_field.h before this
// file. Metal shaders include it by relative path.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_GRAIN_NEGATIVE_INLINE static inline
#elif defined(__METAL_VERSION__)
#define FILM_GRAIN_NEGATIVE_INLINE static inline
#else
#include "edit/runtime/film/film_grain_field.h"
#ifdef __CUDACC__
#define FILM_GRAIN_NEGATIVE_INLINE static __host__ __device__ inline
#else
#define FILM_GRAIN_NEGATIVE_INLINE static inline
#endif
#endif

/// A node above every dye amount: the padding of FilmGrainNodeCurve::x past the counted nodes.
#define FILM_GRAIN_NODE_PAST 3.0e38f

/**
 * One layer curve as its nodes, for a search of the segment. The segment form of
 * FilmGrainCurveSigma adds all 15 segments for each value; in the FP6 kernel of the film print
 * that was about 1.2 ms of the 3 x 15 segments at 6000 x 4000 on CUDA (phase G5). This form reads
 * 4 nodes to find the segment and then one segment, with the same values within the rounding.
 *
 * `x` holds the counted nodes, then FILM_GRAIN_NODE_PAST. `sigma` holds the counted values, then
 * the last value. `inverse_width[i]` is 1 / (x[i + 1] - x[i]) for a counted segment, then 0.
 * `last_segment` is the count - 2. A value below the first node takes the first sigma, and a
 * value above the last node takes the last sigma, as the segment form and the lab (numpy interp).
 */
typedef struct {
  float x[FILM_GRAIN_CURVE_NODES];
  float sigma[FILM_GRAIN_CURVE_NODES];
  float inverse_width[FILM_GRAIN_CURVE_NODES];
  int   last_segment;
} FilmGrainNodeCurve;

/// The value of @p curve at the dye amount @p value.
FILM_GRAIN_NEGATIVE_INLINE float FilmGrainNodeCurveSigma(
    FILM_GRAIN_FIELD_PARAMS FilmGrainNodeCurve* curve, float value) {
  // The largest node at or below the value, in 4 steps over the 16 nodes.
  int segment = 0;
  if (curve->x[segment + 8] <= value) segment += 8;
  if (curve->x[segment + 4] <= value) segment += 4;
  if (curve->x[segment + 2] <= value) segment += 2;
  if (curve->x[segment + 1] <= value) segment += 1;
  segment = segment < curve->last_segment ? segment : curve->last_segment;
  float t = (value - curve->x[segment]) * curve->inverse_width[segment];
  t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return curve->sigma[segment] + (curve->sigma[segment + 1] - curve->sigma[segment]) * t;
}

/**
 * The apply step of one render. The kernels read it through a pointer in their parameter memory
 * (FILM_GRAIN_FIELD_PARAMS), as the post step.
 *
 * The dye amount of layer k is `dye_max[k] + record_sign * record_k`: a negative has
 * `record_sign` 1 and `dye_max` 0; a colour reversal film has `record_sign` -1 and its `dye_max`.
 */
typedef struct {
  float              scale[3];  ///< strength * pixel_factor of each layer
  float              dye_max[3];
  float              record_sign;
  FilmGrainNodeCurve curves[3];  ///< The layer curves of the stock
} FilmGrainNegativeParams;

typedef struct {
  float r;
  float g;
  float b;
} FilmGrainNegativeRgb;

/// One layer: the record @p record and the mixed unit grain @p unit of the layer.
FILM_GRAIN_NEGATIVE_INLINE float FilmGrainNegativeLayer(
    FILM_GRAIN_FIELD_PARAMS FilmGrainNegativeParams* params, int layer, float record, float unit) {
  const float amount = params->dye_max[layer] + params->record_sign * record;
  const float change =
      params->scale[layer] * FilmGrainNodeCurveSigma(&params->curves[layer], amount) * unit;
  return record + params->record_sign * change;
}

/// Adds the grain @p unit (N of rule 'grain_layer_mix') to the record @p record of one pixel.
FILM_GRAIN_NEGATIVE_INLINE FilmGrainNegativeRgb
FilmGrainNegativeRecord(FILM_GRAIN_FIELD_PARAMS FilmGrainNegativeParams* params,
                        FilmGrainNegativeRgb record, FilmGrainNoise3 unit) {
  FilmGrainNegativeRgb result;
  result.r = FilmGrainNegativeLayer(params, 0, record.r, unit.x);
  result.g = FilmGrainNegativeLayer(params, 1, record.g, unit.y);
  result.b = FilmGrainNegativeLayer(params, 2, record.b, unit.z);
  return result;
}

#endif  // ALCEDO_FILM_GRAIN_NEGATIVE_H
