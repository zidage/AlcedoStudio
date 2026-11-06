//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DRT/Post Film Grain on Metal (plan 'film_grain_format_and_stock_plan.md', phase G4): the
// steps of FilmGrainPostKernel of cuda_film_grain_pass.cu and of film_grain_post.cl, in one kernel.
// drt.metal includes this file after color_encoding_math.h; metal_film_grain_pass.mm records it.
//
// Each 16 x 16 threadgroup makes the noise of its tile and a halo of the largest blur radius, at
// the reference pixels of the render pixels, so a viewport region has the grain of the same region
// of the full frame. The threadgroup memory holds two images of 16 x (16 + 2 halo) texels, 3 floats
// each: one band of 16 rows of the noise tile, and the horizontal blur of the tile at its 16
// centre columns. The threadgroup makes the noise one band at a time and blurs each band across.
// At the radius 24 the two images are 24 KB, inside the 32 KB of threadgroup memory of Apple GPUs.
// The pass has no noise or blur image in device memory, so it moves only the source and the
// output pixels.

#include "../../../../include/edit/runtime/film/film_grain_field.h"
#include "../../../../include/edit/runtime/film/film_grain_post.h"

// A size that differs from the host stops the build of this library.
static_assert(sizeof(FilmGrainFieldParams) == 388, "FilmGrainFieldParams differs from the host");
static_assert(sizeof(FilmGrainPostParams) == 612, "FilmGrainPostParams differs from the host");

#define FILM_GRAIN_POST_TILE    16
#define FILM_GRAIN_POST_THREADS (FILM_GRAIN_POST_TILE * FILM_GRAIN_POST_TILE)

/// `FilmGrainPostSize` of metal_film_grain_pass.mm.
struct FilmGrainPostSize {
  uint width;
  uint height;
};

kernel void film_grain_post(texture2d<float, access::read>  src [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            constant FilmGrainFieldParams&  field [[buffer(0)]],
                            constant FilmGrainPostParams&   post [[buffer(1)]],
                            constant FilmGrainPostSize&     size [[buffer(2)]],
                            threadgroup float*              shared [[threadgroup(0)]],
                            uint2 group [[threadgroup_position_in_grid]],
                            uint2 lane2 [[thread_position_in_threadgroup]]) {
  const int                  halo         = field.halo;
  const int                  span         = FILM_GRAIN_POST_TILE + 2 * halo;
  // One band of the noise (16 rows of span columns) and the horizontal blur (span rows of 16
  // columns) have the same number of texels.
  const int                  band         = FILM_GRAIN_POST_TILE * span;
  threadgroup float*         noise        = shared;
  threadgroup float*         across       = shared + 3 * band;
  const int                  local_x      = int(lane2.x);
  const int                  local_y      = int(lane2.y);
  const int                  lane         = local_y * FILM_GRAIN_POST_TILE + local_x;
  const int                  x0           = int(group.x) * FILM_GRAIN_POST_TILE - halo;
  const int                  y0           = int(group.y) * FILM_GRAIN_POST_TILE - halo;
  const FilmGrainNoiseParams noise_params = field.noise;

  for (int band_row = 0; band_row < span; band_row += FILM_GRAIN_POST_TILE) {
    const int rows = min(FILM_GRAIN_POST_TILE, span - band_row);
    // G-noise: one aggregate noise for each texel of the band.
    for (int index = lane; index < rows * span; index += FILM_GRAIN_POST_THREADS) {
      const FilmGrainNoise3 value =
          FilmGrainAggregateNoise(noise_params, x0 + index % span, y0 + band_row + index / span);
      noise[index]            = value.x;
      noise[band + index]     = value.y;
      noise[2 * band + index] = value.z;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    // G-blur-x of the band at the 16 centre columns. The weights past the radius of a channel are
    // 0.
    for (int index = lane; index < rows * FILM_GRAIN_POST_TILE; index += FILM_GRAIN_POST_THREADS) {
      const int row    = index / FILM_GRAIN_POST_TILE;
      const int column = index % FILM_GRAIN_POST_TILE;
      const int centre = row * span + column + halo;
      float     r      = noise[centre] * field.weights[0][0];
      float     g      = noise[band + centre] * field.weights[1][0];
      float     b      = noise[2 * band + centre] * field.weights[2][0];
      for (int tap = 1; tap <= halo; ++tap) {
        r += (noise[centre - tap] + noise[centre + tap]) * field.weights[0][tap];
        g += (noise[band + centre - tap] + noise[band + centre + tap]) * field.weights[1][tap];
        b += (noise[2 * band + centre - tap] + noise[2 * band + centre + tap]) *
             field.weights[2][tap];
      }
      const int out          = (band_row + row) * FILM_GRAIN_POST_TILE + column;
      across[out]            = r;
      across[band + out]     = g;
      across[2 * band + out] = b;
    }
    // The next band overwrites the noise, and the vertical blur reads every band.
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  const int x = int(group.x) * FILM_GRAIN_POST_TILE + local_x;
  const int y = int(group.y) * FILM_GRAIN_POST_TILE + local_y;
  if (x >= int(size.width) || y >= int(size.height)) return;

  // G-blur-y, the unit field, and the mix of rule 'grain_layer_mix'.
  const int centre = (local_y + halo) * FILM_GRAIN_POST_TILE + local_x;
  float     r      = across[centre] * field.weights[0][0];
  float     g      = across[band + centre] * field.weights[1][0];
  float     b      = across[2 * band + centre] * field.weights[2][0];
  for (int tap = 1; tap <= halo; ++tap) {
    const int above = centre - tap * FILM_GRAIN_POST_TILE;
    const int below = centre + tap * FILM_GRAIN_POST_TILE;
    r += (across[above] + across[below]) * field.weights[0][tap];
    g += (across[band + above] + across[band + below]) * field.weights[1][tap];
    b += (across[2 * band + above] + across[2 * band + below]) * field.weights[2][tap];
  }
  FilmGrainNoise3 unit;
  unit.x = r * field.inverse_weight_square_sum[0];
  unit.y = g * field.inverse_weight_square_sum[1];
  unit.z = b * field.inverse_weight_square_sum[2];
  float mix[6];
  for (int entry = 0; entry < 6; ++entry) mix[entry] = field.mix[entry];
  const FilmGrainNoise3     grain = FilmGrainMix(mix, unit);

  const uint2               position    = uint2(uint(x), uint(y));
  const float4              pixel       = src.read(position);
  FilmGrainPostRgb          code;
  code.r                        = pixel.x;
  code.g                        = pixel.y;
  code.b                        = pixel.z;
  const FilmGrainPostRgb result = FilmGrainPostPixel(&post, code, grain);
  dst.write(float4(result.r, result.g, result.b, pixel.w), position);
}
