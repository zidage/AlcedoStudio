// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_GRAIN_FIELD_H
#define ALCEDO_FILM_GRAIN_FIELD_H

// The random field and the stock curve of the film grain (plan
// 'film_grain_format_and_stock_plan.md', sections 3.4 and 6.3). Shared by the host, CUDA, OpenCL C,
// and Metal, so the host tests and the three backends read one hash, one Gaussian, and one curve
// form.
//
// - FilmGrainHash3: pcg3d, the 32-bit hash of Jarzynski and Olano (JCGT 9(3), 2020). One call gives
//   the three channels of one pixel, one per layer. It reads the reference pixel and a seed.
// - FilmGrainGaussian: a unit Gaussian from 32 hash bits, by the single-precision inverse error
//   function of Giles (GPU Computing Gems, 2010). The field is a sum of about 500 dye clouds at
//   each point, so it is Gaussian (section 3.4), and no pass draws one random value per blur tap.
// - FilmGrainCurveSigma: the granularity curve of a stock as literal segments, so no backend
//   indexes an array with a value of the pixel. The host packs the curves (film_grain_plan.hpp).
// - FilmGrainFieldParams, FilmGrainAggregateNoise, FilmGrainMix: the noise pass with its halo, the
//   blur weights of each layer, and the layer mix of section 6.3. The backend kernels run the
//   noise pass, the horizontal blur, and the vertical blur; these functions are their pixel steps.
//
// OpenCL C cannot resolve #include, so the OpenCL program lists this file before the kernels that
// read it. Metal shaders include it by relative path, after <metal_stdlib>.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_GRAIN_FIELD_INLINE static inline
#define FILM_GRAIN_FIELD_UINT   uint
#define FILM_GRAIN_FIELD_LOG    log
#define FILM_GRAIN_FIELD_SQRT   sqrt
#define FILM_GRAIN_FIELD_FLOOR  floor
#elif defined(__METAL_VERSION__)
#define FILM_GRAIN_FIELD_INLINE static inline
#define FILM_GRAIN_FIELD_UINT   uint
#define FILM_GRAIN_FIELD_LOG    log
#define FILM_GRAIN_FIELD_SQRT   sqrt
#define FILM_GRAIN_FIELD_FLOOR  floor
#else
#include <cmath>
#ifdef __CUDACC__
#define FILM_GRAIN_FIELD_INLINE static __host__ __device__ inline
#else
#define FILM_GRAIN_FIELD_INLINE static inline
#endif
#define FILM_GRAIN_FIELD_UINT  unsigned int
#define FILM_GRAIN_FIELD_LOG   logf
#define FILM_GRAIN_FIELD_SQRT  sqrtf
#define FILM_GRAIN_FIELD_FLOOR floorf
#endif

/// Most nodes of one granularity curve of the stock bundle (schema 11, section 4.1).
#define FILM_GRAIN_CURVE_NODES    16
#define FILM_GRAIN_CURVE_SEGMENTS (FILM_GRAIN_CURVE_NODES - 1)

/// The three 32-bit outputs of @ref FilmGrainHash3, one per layer.
typedef struct {
  FILM_GRAIN_FIELD_UINT x;
  FILM_GRAIN_FIELD_UINT y;
  FILM_GRAIN_FIELD_UINT z;
} FilmGrainHashBits;

/**
 * One granularity curve as segments: sigma(v) = first_sigma + sum over i of
 * delta[i] * clamp((v - start[i]) * inverse_width[i], 0, 1).
 *
 * Segment i goes from node i to node i + 1 of the curve. A value below the first node takes the
 * first sigma, and a value above the last node takes the last sigma, as the lab (numpy interp). A
 * segment past the counted nodes has a delta and an inverse width of 0, so it adds 0.
 */
typedef struct {
  float first_sigma;
  float start[FILM_GRAIN_CURVE_SEGMENTS];
  float inverse_width[FILM_GRAIN_CURVE_SEGMENTS];
  float delta[FILM_GRAIN_CURVE_SEGMENTS];
} FilmGrainCurve;

/**
 * pcg3d of Jarzynski and Olano on (x, y, seed). @p x and @p y are the coordinates of a reference
 * pixel, so the field of a region is the same in every render that reads that region.
 */
FILM_GRAIN_FIELD_INLINE FilmGrainHashBits FilmGrainHash3(FILM_GRAIN_FIELD_UINT x,
                                                         FILM_GRAIN_FIELD_UINT y,
                                                         FILM_GRAIN_FIELD_UINT seed) {
  FILM_GRAIN_FIELD_UINT a = x * 1664525u + 1013904223u;
  FILM_GRAIN_FIELD_UINT b = y * 1664525u + 1013904223u;
  FILM_GRAIN_FIELD_UINT c = seed * 1664525u + 1013904223u;
  a += b * c;
  b += c * a;
  c += a * b;
  a ^= a >> 16u;
  b ^= b >> 16u;
  c ^= c >> 16u;
  a += b * c;
  b += c * a;
  c += a * b;
  FilmGrainHashBits bits;
  bits.x = a;
  bits.y = b;
  bits.z = c;
  return bits;
}

/**
 * A unit Gaussian from the 32 bits of a hash.
 *
 * The 23 high bits give u = (k + 0.5) / 2^23 in [2^-24, 1 - 2^-24], exact in single precision, so
 * 1 - u is exact and the tails end at about 5.3. The result is sqrt(2) erfinv(2 u - 1), with
 * 4 u (1 - u) in place of (1 - x)(1 + x) for no cancellation near the tails.
 */
FILM_GRAIN_FIELD_INLINE float FilmGrainGaussian(FILM_GRAIN_FIELD_UINT bits) {
  const float u = (float)(bits >> 9u) * (1.0f / 8388608.0f) + (1.0f / 16777216.0f);
  const float x = 2.0f * u - 1.0f;
  float       w = -FILM_GRAIN_FIELD_LOG(4.0f * u * (1.0f - u));
  float       p;
  if (w < 5.0f) {
    w = w - 2.5f;
    p = 2.81022636e-08f;
    p = 3.43273939e-07f + p * w;
    p = -3.5233877e-06f + p * w;
    p = -4.39150654e-06f + p * w;
    p = 0.00021858087f + p * w;
    p = -0.00125372503f + p * w;
    p = -0.00417768164f + p * w;
    p = 0.246640727f + p * w;
    p = 1.50140941f + p * w;
  } else {
    w = FILM_GRAIN_FIELD_SQRT(w) - 3.0f;
    p = -0.000200214257f;
    p = 0.000100950558f + p * w;
    p = 0.00134934322f + p * w;
    p = -0.00367342844f + p * w;
    p = 0.00573950773f + p * w;
    p = -0.0076224613f + p * w;
    p = 0.00943887047f + p * w;
    p = 1.00167406f + p * w;
    p = 2.83297682f + p * w;
  }
  return 1.41421356f * p * x;
}

/// The value of @p curve at @p value: a dye amount (layer curve) or a gross density (channel
/// curve).
FILM_GRAIN_FIELD_INLINE float FilmGrainCurveSigma(FilmGrainCurve curve, float value) {
  float sigma = curve.first_sigma;
  for (int segment = 0; segment < FILM_GRAIN_CURVE_SEGMENTS; ++segment) {
    float t = (value - curve.start[segment]) * curve.inverse_width[segment];
    t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    sigma += curve.delta[segment] * t;
  }
  return sigma;
}

/// Largest blur radius of rule 'grain_kernel', in taps on each side of the centre.
#define FILM_GRAIN_MAX_BLUR_RADIUS 24
/// Largest k of rule 'grain_aggregate'.
#define FILM_GRAIN_MAX_AGGREGATE   4

/**
 * The reference noise of one render (rules 'grain_aggregate' and 'grain_pitch'). The host fills it
 * from the render geometry (film_grain_plan.hpp, `MakeFilmGrainFieldParams`).
 *
 * `render_to_reference` maps a render pixel position to a reference pixel position:
 * reference = m * (render + 0.5) + t - 0.5, with the pixel centres on integers. The noise reads the
 * reference pixels for every render: the full frame, the fit view, and a viewport region. So a
 * region has the same grain in every render that reads it, and a pan moves the grain with the
 * image.
 */
typedef struct {
  float                 render_to_reference[6];  ///< m00 m01 tx m10 m11 ty
  FILM_GRAIN_FIELD_UINT seed;
  /// k: reference pixels summed per render pixel and axis, 1 to FILM_GRAIN_MAX_AGGREGATE.
  FILM_GRAIN_FIELD_UINT aggregate;
  float                 inverse_aggregate;  ///< 1 / k
} FilmGrainNoiseParams;

/**
 * The noise, the blur, and the mix of one render (rules 'grain_kernel' and 'grain_layer_mix').
 * Copied by value into the kernels.
 *
 * The noise image covers the render plus `halo` pixels on each side. The blur of channel k has
 * the radius `radius[k]` and the weights `weights[k][0]` (centre) to `weights[k][radius[k]]`. The
 * blurred field divided by its sum of w^2 has unit variance (`inverse_weight_square_sum`). `mix`
 * is the lower Cholesky factor L of the correlation, row major below the diagonal: l00, l10, l11,
 * l20, l21, l22. The grain of channel j is N_j = sum over k of L_jk u_k.
 */
typedef struct {
  FilmGrainNoiseParams noise;
  int                  halo;  ///< The largest radius of the three channels
  int                  radius[3];
  float                weights[3][FILM_GRAIN_MAX_BLUR_RADIUS + 1];
  float                inverse_weight_square_sum[3];
  float                mix[6];
} FilmGrainFieldParams;

/// The three unit Gaussians of one pixel, one per layer.
typedef struct {
  float x;
  float y;
  float z;
} FilmGrainNoise3;

/**
 * The noise of the render pixel (@p x, @p y), which can be outside the render (the halo): the sum
 * of the k x k reference-pixel Gaussians at its reference position, divided by k. The result has
 * unit variance, and its 2 x 2 box mean at k = 1 equals the noise of the render at half size at
 * k = 2, so the fit view reads the grain of the downscaled full image (issue 674).
 */
FILM_GRAIN_FIELD_INLINE FilmGrainNoise3 FilmGrainAggregateNoise(FilmGrainNoiseParams params, int x,
                                                                int y) {
  const float px = (float)x + 0.5f;
  const float py = (float)y + 0.5f;
  const float rx = params.render_to_reference[0] * px + params.render_to_reference[1] * py +
                   params.render_to_reference[2] - 0.5f;
  const float ry = params.render_to_reference[3] * px + params.render_to_reference[4] * py +
                   params.render_to_reference[5] - 0.5f;
  const int       k      = (int)params.aggregate;
  // The k reference pixels centred on the reference position: for k = 2 and a render at half size,
  // the reference pixels 2x and 2x + 1.
  const float     offset = 0.5f * (float)(k - 1) - 0.5f;
  const int       base_x = (int)FILM_GRAIN_FIELD_FLOOR(rx - offset);
  const int       base_y = (int)FILM_GRAIN_FIELD_FLOOR(ry - offset);
  FilmGrainNoise3 sum;
  sum.x = 0.0f;
  sum.y = 0.0f;
  sum.z = 0.0f;
  for (int j = 0; j < k; ++j) {
    for (int i = 0; i < k; ++i) {
      const FilmGrainHashBits bits = FilmGrainHash3(
          (FILM_GRAIN_FIELD_UINT)(base_x + i), (FILM_GRAIN_FIELD_UINT)(base_y + j), params.seed);
      sum.x += FilmGrainGaussian(bits.x);
      sum.y += FilmGrainGaussian(bits.y);
      sum.z += FilmGrainGaussian(bits.z);
    }
  }
  sum.x *= params.inverse_aggregate;
  sum.y *= params.inverse_aggregate;
  sum.z *= params.inverse_aggregate;
  return sum;
}

/// Rule 'grain_layer_mix': N = L u for the unit fields @p unit of the three layers.
FILM_GRAIN_FIELD_INLINE FilmGrainNoise3 FilmGrainMix(const float mix[6], FilmGrainNoise3 unit) {
  FilmGrainNoise3 mixed;
  mixed.x = mix[0] * unit.x;
  mixed.y = mix[1] * unit.x + mix[2] * unit.y;
  mixed.z = mix[3] * unit.x + mix[4] * unit.y + mix[5] * unit.z;
  return mixed;
}

#endif  // ALCEDO_FILM_GRAIN_FIELD_H
