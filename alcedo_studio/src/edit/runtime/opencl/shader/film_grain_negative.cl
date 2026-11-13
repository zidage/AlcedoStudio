//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The grain in the negative of the film print on OpenCL, alone (plan
// 'film_grain_format_and_stock_plan.md', phase G6): the steps of film_grain_tile.h with the apply
// step of film_grain_negative.h, on an image of dye amounts, in place. The film print runs the same
// steps inside its FP6 kernels (film_print_display_grain, film_print_scan_display_grain) and writes
// no amounts with grain; this kernel gives the step alone, so the backend tests measure the dye
// amounts of each stock. The program lists film_grain_field.h, film_grain_tile.h, and
// film_grain_negative.h before this file.

/// @p amounts holds 3 floats per pixel from @p amounts_offset, row by row: the record of each layer
/// after the inhibitor step. @p data holds the FilmGrainFieldParams at @p field_offset and the
/// FilmGrainNegativeParams at @p negative_offset; the work-group copies them into local memory.
/// Each lane reads and writes only its own pixels.
__kernel __attribute__((reqd_work_group_size(FILM_GRAIN_TILE, FILM_GRAIN_TILE, 1))) void
film_grain_negative_amounts(__global float* amounts, uint amounts_offset, uint width, uint height,
                            __global const uchar* data, uint field_offset, uint negative_offset,
                            __local float* shared) {
  const int                       local_x = (int)get_local_id(0);
  const int                       local_y = (int)get_local_id(1);
  const int                       lane    = local_y * FILM_GRAIN_TILE + local_x;
  __local FilmGrainFieldParams    field;
  __local FilmGrainNegativeParams negative;
  FilmGrainTileCopyWords(data + field_offset, &field, sizeof(FilmGrainFieldParams), lane);
  FilmGrainTileCopyWords(data + negative_offset, &negative, sizeof(FilmGrainNegativeParams), lane);
  // Every lane reads these before the first barrier, so they come from the upload.
  __global const FilmGrainFieldParams* upload =
      (__global const FilmGrainFieldParams*)(data + field_offset);
  const int halo = upload->halo;
  FilmGrainTileAcross(&field, upload->noise, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS,
                      (int)get_group_id(0), (int)get_group_id(1), lane, shared);

  const int x = (int)get_group_id(0) * FILM_GRAIN_TILE + local_x;
  if (x >= (int)width) return;
  for (int half_index = 0; half_index < FILM_GRAIN_NEGATIVE_TILE_ROWS / FILM_GRAIN_TILE;
       ++half_index) {
    const int tile_row = local_y + half_index * FILM_GRAIN_TILE;
    const int y        = (int)get_group_id(1) * FILM_GRAIN_NEGATIVE_TILE_ROWS + tile_row;
    if (y >= (int)height) return;
    const FilmGrainNoise3 grain =
        FilmGrainTileGrain(&field, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS, tile_row, local_x, shared);
    __global float*      texel = amounts + amounts_offset + 3U * ((uint)y * width + (uint)x);
    FilmGrainNegativeRgb record;
    record.r                          = texel[0];
    record.g                          = texel[1];
    record.b                          = texel[2];
    const FilmGrainNegativeRgb result = FilmGrainNegativeRecord(&negative, record, grain);
    texel[0]                          = result.r;
    texel[1]                          = result.g;
    texel[2]                          = result.b;
  }
}
