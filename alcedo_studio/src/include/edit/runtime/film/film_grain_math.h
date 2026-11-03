// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_GRAIN_MATH_H
#define ALCEDO_FILM_GRAIN_MATH_H

// Dye clouds of the DRT/Post film grain: the step that turns the blurred grain coverage of each
// channel into the display value with grain. Shared by the host, CUDA, OpenCL C, and Metal, so the
// three backends and the host tests read one set of constants.
//
// Inputs, for each channel: the display value `source` in 0 to 1, and `coverage`, the 7-tap blur
// of a Bernoulli draw with the probability `source` (the horizontal pass of each backend). The
// deviation coverage - source is the dye-cloud field of that channel. The three fields are
// independent and have the same spatial form.
//
// Colour of the grain. The fields are made unit variance, mixed by a fixed 3x3 matrix, and scaled
// by the datasheet granularity of each layer and a fixed weight of each channel:
// - The mix gives the channel correlation of a grain whose variance is 57 % on the neutral axis,
//   27 % on the warm-cool axis (r - b), and 16 % on the green-magenta axis (2g - r - b).
// - The channel weights set the mid-grey amplitude to about r:g:b = 1.2 : 1 : 1.4.
// Both values come from measurements of real colour negative grain: a Flextight scan of Kodak 5207
// and lab scans of Kodak Pro Image 100, on flat areas, with scanner white noise excluded. Both
// films have the warm-cool axis as the main chroma axis and the smallest grain in green.
// The grain keeps its colour up to white; only the brightening half of the grain gets smaller in
// the highlights, so the grain does not push a highlight past white.
//
// OpenCL C cannot resolve #include, so the OpenCL program lists this file before the neighbor
// kernels (opencl_gpu_dag_programs.cpp). Metal shaders include it by relative path.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_GRAIN_INLINE static inline
#define FILM_GRAIN_SQRT   sqrt
#elif defined(__METAL_VERSION__)
#define FILM_GRAIN_INLINE static inline
#define FILM_GRAIN_SQRT   sqrt
#else
#include <cmath>
#ifdef __CUDACC__
#define FILM_GRAIN_INLINE static __host__ __device__ inline
#else
#define FILM_GRAIN_INLINE static inline
#endif
#define FILM_GRAIN_SQRT sqrtf
#endif

typedef struct {
  float r;
  float g;
  float b;
} FilmGrainRgb;

/// Lower limit of p (1 - p) when a field is made unit variance. Near 0 and 1 the Bernoulli field is
/// sparse; the limit keeps its unit form finite. The grain there is small because the amplitude
/// uses the true p (1 - p).
#define FILM_GRAIN_UNIT_VARIANCE_FLOOR 0.03f

FILM_GRAIN_INLINE float FilmGrainClamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

FILM_GRAIN_INLINE float FilmGrainLerp(float a, float b, float t) { return a + (b - a) * t; }

FILM_GRAIN_INLINE float FilmGrainSmoothstep(float edge0, float edge1, float value) {
  const float t = FilmGrainClamp01((value - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

/// One segment of a piecewise-linear curve: the change @p delta spread over [d0, d1].
FILM_GRAIN_INLINE float FilmGrainSegment(float density, float d0, float d1, float delta) {
  return delta * FilmGrainClamp01((density - d0) / (d1 - d0));
}

/**
 * RMS granularity (sigma D) of one layer at a density, from the Kodak 5207 datasheet curves.
 * Written as segments with literal values, so no backend indexes an array per pixel.
 */
FILM_GRAIN_INLINE float FilmGrainLayerSigma(float density, int channel) {
  if (channel == 0) {
    return 0.00565f + FilmGrainSegment(density, 0.22f, 0.25f, 0.00524f - 0.00565f) +
           FilmGrainSegment(density, 0.25f, 0.42f, 0.01085f - 0.00524f) +
           FilmGrainSegment(density, 0.42f, 0.78f, 0.00844f - 0.01085f) +
           FilmGrainSegment(density, 0.78f, 1.19f, 0.00531f - 0.00844f) +
           FilmGrainSegment(density, 1.19f, 1.58f, 0.00486f - 0.00531f) +
           FilmGrainSegment(density, 1.94f, 2.26f, 0.00445f - 0.00486f) +
           FilmGrainSegment(density, 2.26f, 2.45f, 0.00440f - 0.00445f) +
           FilmGrainSegment(density, 2.45f, 2.52f, 0.00474f - 0.00440f);
  }
  if (channel == 1) {
    return 0.00517f + FilmGrainSegment(density, 0.59f, 0.61f, 0.00524f - 0.00517f) +
           FilmGrainSegment(density, 0.61f, 0.66f, 0.00625f - 0.00524f) +
           FilmGrainSegment(density, 0.66f, 0.94f, 0.01085f - 0.00625f) +
           FilmGrainSegment(density, 0.94f, 1.36f, 0.00823f - 0.01085f) +
           FilmGrainSegment(density, 1.36f, 1.76f, 0.00617f - 0.00823f) +
           FilmGrainSegment(density, 1.76f, 2.18f, 0.00625f - 0.00617f) +
           FilmGrainSegment(density, 2.18f, 2.49f, 0.00691f - 0.00625f) +
           FilmGrainSegment(density, 2.49f, 2.61f, 0.00602f - 0.00691f) +
           FilmGrainSegment(density, 2.61f, 2.67f, 0.00524f - 0.00602f) +
           FilmGrainSegment(density, 2.67f, 2.69f, 0.00445f - 0.00524f);
  }
  return 0.01185f + FilmGrainSegment(density, 1.00f, 1.03f, 0.01261f - 0.01185f) +
         FilmGrainSegment(density, 1.03f, 1.10f, 0.01485f - 0.01261f) +
         FilmGrainSegment(density, 1.10f, 1.32f, 0.01581f - 0.01485f) +
         FilmGrainSegment(density, 1.32f, 1.51f, 0.01200f - 0.01581f) +
         FilmGrainSegment(density, 1.51f, 1.78f, 0.01099f - 0.01200f) +
         FilmGrainSegment(density, 1.78f, 2.05f, 0.01127f - 0.01099f) +
         FilmGrainSegment(density, 2.05f, 2.38f, 0.01058f - 0.01127f) +
         FilmGrainSegment(density, 2.38f, 2.68f, 0.00844f - 0.01058f) +
         FilmGrainSegment(density, 2.68f, 2.91f, 0.00641f - 0.00844f) +
         FilmGrainSegment(density, 2.91f, 3.00f, 0.00418f - 0.00641f);
}

/// Granularity of a channel relative to sigma D 0.0075, at the layer density of the display value.
FILM_GRAIN_INLINE float FilmGrainGranularityScale(float signal, int channel) {
  const float u       = FilmGrainClamp01(signal);
  const float density = channel == 0   ? FilmGrainLerp(0.22f, 2.52f, u)
                        : channel == 1 ? FilmGrainLerp(0.59f, 2.69f, u)
                                       : FilmGrainLerp(1.00f, 3.00f, u);
  const float scale   = FilmGrainLayerSigma(density, channel) * (1.0f / 0.0075f);
  return scale < 0.55f ? 0.55f : (scale > 2.15f ? 2.15f : scale);
}

/// The dye-cloud field of a channel at unit variance (before the 7-tap blur factor).
FILM_GRAIN_INLINE float FilmGrainUnitField(float probability, float coverage) {
  const float variance = probability * (1.0f - probability);
  const float limited =
      variance > FILM_GRAIN_UNIT_VARIANCE_FLOOR ? variance : FILM_GRAIN_UNIT_VARIANCE_FLOOR;
  return (coverage - probability) / FILM_GRAIN_SQRT(limited);
}

/**
 * Applies the dye clouds to a display value. @p amount is the Film Grain value / 3.
 *
 * Mix: the symmetric square root of the channel correlation [[1, 0.4231, 0.2322], [0.4231, 1,
 * 0.4231], [0.2322, 0.4231, 1]], so no channel is the primary one. Each row has unit norm, so each
 * channel keeps the variance of its own field.
 *
 * Channel scale: 0.80100 (the per-channel amplitude of the previous neutral/chroma mix, so the
 * slider strength stays) times the channel weights 1.3463, 0.8639, 0.9026.
 */
FILM_GRAIN_INLINE FilmGrainRgb FilmGrainDyeClouds(FilmGrainRgb source, FilmGrainRgb coverage,
                                                  float amount) {
  const float pr      = FilmGrainClamp01(source.r);
  const float pg      = FilmGrainClamp01(source.g);
  const float pb      = FilmGrainClamp01(source.b);
  const float ur      = FilmGrainUnitField(pr, coverage.r);
  const float ug      = FilmGrainUnitField(pg, coverage.g);
  const float ub      = FilmGrainUnitField(pb, coverage.b);

  const float mixed_r = 0.97312296f * ur + 0.20890844f * ug + 0.09689667f * ub;
  const float mixed_g = 0.20890844f * ur + 0.95536094f * ug + 0.20890844f * ub;
  const float mixed_b = 0.09689667f * ur + 0.20890844f * ug + 0.97312296f * ub;

  float       error_r = mixed_r * FILM_GRAIN_SQRT(pr * (1.0f - pr)) *
                  FilmGrainGranularityScale(source.r, 0) * (0.80099938f * 1.3463f);
  float error_g = mixed_g * FILM_GRAIN_SQRT(pg * (1.0f - pg)) *
                  FilmGrainGranularityScale(source.g, 1) * (0.80099938f * 0.8639f);
  float error_b = mixed_b * FILM_GRAIN_SQRT(pb * (1.0f - pb)) *
                  FilmGrainGranularityScale(source.b, 2) * (0.80099938f * 0.9026f);

  // A negative density error brightens. Near white it gets smaller, so a highlight stays below
  // white; the grain keeps its colour.
  const float highlight = FilmGrainSmoothstep(0.72f, 0.96f, (pr + pg + pb) * (1.0f / 3.0f));
  const float brighten  = FilmGrainLerp(1.0f, 0.12f, highlight);
  error_r               = error_r < 0.0f ? error_r * brighten : error_r;
  error_g               = error_g < 0.0f ? error_g * brighten : error_g;
  error_b               = error_b < 0.0f ? error_b * brighten : error_b;

  const float  strength = amount * 1.35f;
  FilmGrainRgb result;
  result.r = source.r - source.r * strength * error_r;
  result.g = source.g - source.g * strength * error_g;
  result.b = source.b - source.b * strength * error_b;
  return result;
}

#endif  // ALCEDO_FILM_GRAIN_MATH_H
