// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_GRAIN_TILE_H
#define ALCEDO_FILM_GRAIN_TILE_H

// The tile steps of the film grain on OpenCL and Metal (plan 'film_grain_format_and_stock_plan.md',
// section 6.3): the noise of one tile with its halo (G-noise), the horizontal blur (G-blur-x), and
// at each pixel the vertical blur, the unit field, and the mix (G-blur-y). They are the steps of
// FilmGrainTileKernel of cuda_film_grain_tile.cuh, with the same order of each sum. The DRT/Post
// Film Grain (film_grain_post.cl, film_grain_post.metal, phase G4) and the FP6 kernels of the film
// print (film_print.cl, film_print_scan.cl, film_print.metal, film_print_scan.metal, phase G6) call
// them with their own apply step, so every grain on these backends reads one field.
//
// A work-group (OpenCL) or a threadgroup (Metal) has 16 x 16 lanes and covers a tile of 16 columns
// and `tile_rows` rows (16 or 32). With 32 rows each lane applies the grain to the pixels of rows
// r and r + 16, so the noise of the halo and the horizontal blur serve twice as many pixels, as in
// the FP6 kernels of CUDA. The shared memory holds two images, three planes each, one per channel:
// - `noise`, one band of FilmGrainTileBandRows rows of the noise tile, all its 16 + 2 halo columns;
// - `across`, the horizontal blur of the tile at its 16 centre columns, all its tile_rows + 2 halo
//   rows.
// The group makes the noise one band at a time and blurs each band across. One band holds the
// whole tile when the two images fit in FILM_GRAIN_TILE_SHARED_LIMIT bytes, else a band has 16
// rows. The largest layout (32 rows at the halo 8, or the radius 24) is 27 KB, so with the about
// 1 KB of parameters that OpenCL copies into local memory it stays inside the 32 KB of local memory
// that OpenCL 1.2 gives every device and of threadgroup memory of Apple GPUs.
//
// The host includes this file for the layout functions (the size of the shared memory). OpenCL C
// cannot resolve #include, so an OpenCL program lists film_grain_field.h before this file. Metal
// shaders include it by relative path, after film_grain_field.h.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_GRAIN_TILE_INLINE    static inline
#define FILM_GRAIN_TILE_SHARED    __local
#define FILM_GRAIN_TILE_BARRIER() barrier(CLK_LOCAL_MEM_FENCE)
#define FILM_GRAIN_TILE_DEVICE    1
#elif defined(__METAL_VERSION__)
#define FILM_GRAIN_TILE_INLINE    static inline
#define FILM_GRAIN_TILE_SHARED    threadgroup
#define FILM_GRAIN_TILE_BARRIER() threadgroup_barrier(mem_flags::mem_threadgroup)
#define FILM_GRAIN_TILE_DEVICE    1
#else
#include "edit/runtime/film/film_grain_field.h"
#define FILM_GRAIN_TILE_INLINE static inline
#endif

/// Columns of a tile, and lanes of a group on each axis.
#define FILM_GRAIN_TILE               16
#define FILM_GRAIN_TILE_THREADS       (FILM_GRAIN_TILE * FILM_GRAIN_TILE)
/// Bytes of the two shared images when one band holds the whole tile.
#define FILM_GRAIN_TILE_SHARED_LIMIT  28672
/// Rows of the tile of the post grain, and of the grain in the negative of the film print. The post
/// grain keeps 16 rows: with 32 rows its apply step took twice as long on CUDA (phase G5).
#define FILM_GRAIN_POST_TILE_ROWS     FILM_GRAIN_TILE
#define FILM_GRAIN_NEGATIVE_TILE_ROWS (2 * FILM_GRAIN_TILE)

/// Floats of the two shared images of a tile of @p tile_rows rows, with bands of @p band_rows rows.
FILM_GRAIN_TILE_INLINE int FilmGrainTileSharedFloatsFor(int halo, int tile_rows, int band_rows) {
  return 3 * (band_rows * (FILM_GRAIN_TILE + 2 * halo) + (tile_rows + 2 * halo) * FILM_GRAIN_TILE);
}

/// Rows of one noise band: the whole tile with its halo when the shared images then fit in
/// FILM_GRAIN_TILE_SHARED_LIMIT bytes, else 16. One band has one barrier pair.
FILM_GRAIN_TILE_INLINE int FilmGrainTileBandRows(int halo, int tile_rows) {
  const int rows = tile_rows + 2 * halo;
  return FilmGrainTileSharedFloatsFor(halo, tile_rows, rows) * 4 <= FILM_GRAIN_TILE_SHARED_LIMIT
             ? rows
             : FILM_GRAIN_TILE;
}

/// Floats of the shared memory of one group: the argument size of the host.
FILM_GRAIN_TILE_INLINE int FilmGrainTileSharedFloats(int halo, int tile_rows) {
  return FilmGrainTileSharedFloatsFor(halo, tile_rows, FilmGrainTileBandRows(halo, tile_rows));
}

#ifdef FILM_GRAIN_TILE_DEVICE

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
/// Copies @p count 32-bit words of an upload into local memory, each lane of the work-group a share
/// of them. An OpenCL grain kernel copies its parameters (above the 1024 bytes of by-value
/// arguments of OpenCL 1.2) once for the group: its loops read them for every tap and pixel.
static inline void FilmGrainTileCopyWords(__global const uchar* from, __local void* to, uint bytes,
                                          int lane) {
  __global const uint* source = (__global const uint*)from;
  __local uint*        target = (__local uint*)to;
  const int            count  = (int)(bytes / 4U);
  for (int word = lane; word < count; word += FILM_GRAIN_TILE_THREADS) target[word] = source[word];
}
#endif

/**
 * G-noise and G-blur-x of the tile of group (@p group_x, @p group_y) into the `across` image of
 * @p shared. Every lane of the group calls it: it has the barriers of the bands.
 *
 * @p noise_params and @p halo are values that the lane read before the call: on OpenCL the group
 * copies @p field into local memory, and the first barrier of this function publishes the copy.
 * The noise reads the reference pixels of the render pixels, so a tile at the edge of a viewport
 * region reads the same noise as in the full frame. The weights past the radius of a channel are 0.
 */
FILM_GRAIN_TILE_INLINE void FilmGrainTileAcross(FILM_GRAIN_FIELD_PARAMS FilmGrainFieldParams* field,
                                                FilmGrainNoiseParams noise_params, int halo,
                                                int tile_rows, int group_x, int group_y, int lane,
                                                FILM_GRAIN_TILE_SHARED float* shared) {
  const int                     columns   = FILM_GRAIN_TILE + 2 * halo;
  const int                     span_rows = tile_rows + 2 * halo;
  const int                     band_rows = FilmGrainTileBandRows(halo, tile_rows);
  const int                     plane     = band_rows * columns;
  const int                     band      = FILM_GRAIN_TILE * span_rows;
  FILM_GRAIN_TILE_SHARED float* noise     = shared;
  FILM_GRAIN_TILE_SHARED float* across    = shared + 3 * plane;
  const int                     x0        = group_x * FILM_GRAIN_TILE - halo;
  const int                     y0        = group_y * tile_rows - halo;
  for (int band_row = 0; band_row < span_rows; band_row += band_rows) {
    const int rows = min(band_rows, span_rows - band_row);
    // G-noise: one aggregate noise for each texel of the band.
    for (int index = lane; index < rows * columns; index += FILM_GRAIN_TILE_THREADS) {
      const FilmGrainNoise3 value = FilmGrainAggregateNoise(noise_params, x0 + index % columns,
                                                            y0 + band_row + index / columns);
      noise[index]                = value.x;
      noise[plane + index]        = value.y;
      noise[2 * plane + index]    = value.z;
    }
    FILM_GRAIN_TILE_BARRIER();
    // G-blur-x of the band at the 16 centre columns.
    for (int index = lane; index < rows * FILM_GRAIN_TILE; index += FILM_GRAIN_TILE_THREADS) {
      const int row    = index / FILM_GRAIN_TILE;
      const int column = index % FILM_GRAIN_TILE;
      const int centre = row * columns + column + halo;
      float     r      = noise[centre] * field->weights[0][0];
      float     g      = noise[plane + centre] * field->weights[1][0];
      float     b      = noise[2 * plane + centre] * field->weights[2][0];
      for (int tap = 1; tap <= halo; ++tap) {
        r += (noise[centre - tap] + noise[centre + tap]) * field->weights[0][tap];
        g += (noise[plane + centre - tap] + noise[plane + centre + tap]) * field->weights[1][tap];
        b += (noise[2 * plane + centre - tap] + noise[2 * plane + centre + tap]) *
             field->weights[2][tap];
      }
      const int out          = (band_row + row) * FILM_GRAIN_TILE + column;
      across[out]            = r;
      across[band + out]     = g;
      across[2 * band + out] = b;
    }
    // The next band overwrites the noise, and the vertical blur reads every band.
    FILM_GRAIN_TILE_BARRIER();
  }
}

/**
 * G-blur-y, the unit field, and the mix of rule 'grain_layer_mix' at row @p tile_row and column
 * @p column of the tile, after @ref FilmGrainTileAcross. Returns the mixed unit grain N of the
 * pixel.
 */
FILM_GRAIN_TILE_INLINE FilmGrainNoise3
FilmGrainTileGrain(FILM_GRAIN_FIELD_PARAMS FilmGrainFieldParams* field, int halo, int tile_rows,
                   int tile_row, int column, FILM_GRAIN_TILE_SHARED const float* shared) {
  const int                           columns   = FILM_GRAIN_TILE + 2 * halo;
  const int                           span_rows = tile_rows + 2 * halo;
  const int                           plane     = FilmGrainTileBandRows(halo, tile_rows) * columns;
  const int                           band      = FILM_GRAIN_TILE * span_rows;
  FILM_GRAIN_TILE_SHARED const float* across    = shared + 3 * plane;
  const int                           centre    = (tile_row + halo) * FILM_GRAIN_TILE + column;
  float                               r         = across[centre] * field->weights[0][0];
  float                               g         = across[band + centre] * field->weights[1][0];
  float                               b         = across[2 * band + centre] * field->weights[2][0];
  for (int tap = 1; tap <= halo; ++tap) {
    const int above = centre - tap * FILM_GRAIN_TILE;
    const int below = centre + tap * FILM_GRAIN_TILE;
    r += (across[above] + across[below]) * field->weights[0][tap];
    g += (across[band + above] + across[band + below]) * field->weights[1][tap];
    b += (across[2 * band + above] + across[2 * band + below]) * field->weights[2][tap];
  }
  FilmGrainNoise3 unit;
  unit.x = r * field->inverse_weight_square_sum[0];
  unit.y = g * field->inverse_weight_square_sum[1];
  unit.z = b * field->inverse_weight_square_sum[2];
  float mix[6];
  for (int entry = 0; entry < 6; ++entry) mix[entry] = field->mix[entry];
  return FilmGrainMix(mix, unit);
}

#endif  // FILM_GRAIN_TILE_DEVICE

#endif  // ALCEDO_FILM_GRAIN_TILE_H
