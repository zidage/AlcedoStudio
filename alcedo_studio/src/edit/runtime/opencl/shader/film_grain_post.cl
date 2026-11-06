//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DRT/Post Film Grain on OpenCL (plan 'film_grain_format_and_stock_plan.md', phase G4): the
// steps of FilmGrainPostKernel of cuda_film_grain_pass.cu, in one kernel. The program lists
// color_encoding_math.h, film_grain_field.h, and film_grain_post.h before this file.
//
// Each 16 x 16 work-group makes the noise of its tile and a halo of the largest blur radius, at the
// reference pixels of the render pixels, so a viewport region has the grain of the same region of
// the full frame. The local memory holds two images of 16 x (16 + 2 halo) texels, 3 floats each:
// - `noise`, one band of 16 rows of the noise tile, all its columns;
// - `across`, the horizontal blur of the tile at its 16 centre columns, all rows of the tile.
// The work-group makes the noise one band at a time, and blurs each band across into `across`. At
// the radius 24 the two images are 24 KB, inside the 32 KB of local memory that OpenCL 1.2 gives
// every device.
//
// The fixed values of the render are one upload of FilmGrainPostDeviceData of
// opencl_film_grain_pass.cpp: a FilmGrainFieldParams at `field_offset` and a FilmGrainPostParams
// at `post_offset`, in bytes from `data`. They are too large for by-value arguments of OpenCL 1.2.

#define FILM_GRAIN_POST_TILE    16
#define FILM_GRAIN_POST_THREADS (FILM_GRAIN_POST_TILE * FILM_GRAIN_POST_TILE)

__constant sampler_t kFilmGrainPostSampler =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

static inline float4 FilmGrainPostRead(read_only image2d_t image, __global const float4* buffer,
                                       int is_buffer, int x, int y, uint width) {
  if (is_buffer != 0) {
    return buffer[(uint)y * width + (uint)x];
  }
  return read_imagef(image, kFilmGrainPostSampler, (int2)(x, y));
}

__kernel __attribute__((reqd_work_group_size(FILM_GRAIN_POST_TILE, FILM_GRAIN_POST_TILE, 1))) void
film_grain_post_rgba32f(read_only image2d_t src_image, __global const float4* src_buffer,
                        int src_is_buffer, write_only image2d_t dst_image,
                        __global float4* dst_buffer, int dst_is_buffer, __global const uchar* data,
                        uint field_offset, uint post_offset, uint width, uint height,
                        __local float* shared) {
  __global const FilmGrainFieldParams* field =
      (__global const FilmGrainFieldParams*)(data + field_offset);
  const int                  halo   = field->halo;
  const int                  span   = FILM_GRAIN_POST_TILE + 2 * halo;
  // One band of the noise (16 rows of span columns) and the horizontal blur (span rows of 16
  // columns) have the same number of texels.
  const int                  band   = FILM_GRAIN_POST_TILE * span;
  __local float*             noise  = shared;
  __local float*             across = shared + 3 * band;
  const int                  local_x = (int)get_local_id(0);
  const int                  local_y = (int)get_local_id(1);
  const int                  lane    = local_y * FILM_GRAIN_POST_TILE + local_x;
  const int                  x0      = (int)get_group_id(0) * FILM_GRAIN_POST_TILE - halo;
  const int                  y0      = (int)get_group_id(1) * FILM_GRAIN_POST_TILE - halo;
  const FilmGrainNoiseParams noise_params = field->noise;

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
    barrier(CLK_LOCAL_MEM_FENCE);
    // G-blur-x of the band at the 16 centre columns. The weights past the radius of a channel are
    // 0.
    for (int index = lane; index < rows * FILM_GRAIN_POST_TILE; index += FILM_GRAIN_POST_THREADS) {
      const int row    = index / FILM_GRAIN_POST_TILE;
      const int column = index % FILM_GRAIN_POST_TILE;
      const int centre = row * span + column + halo;
      float     r      = noise[centre] * field->weights[0][0];
      float     g      = noise[band + centre] * field->weights[1][0];
      float     b      = noise[2 * band + centre] * field->weights[2][0];
      for (int tap = 1; tap <= halo; ++tap) {
        r += (noise[centre - tap] + noise[centre + tap]) * field->weights[0][tap];
        g += (noise[band + centre - tap] + noise[band + centre + tap]) * field->weights[1][tap];
        b += (noise[2 * band + centre - tap] + noise[2 * band + centre + tap]) *
             field->weights[2][tap];
      }
      const int out             = (band_row + row) * FILM_GRAIN_POST_TILE + column;
      across[out]               = r;
      across[band + out]        = g;
      across[2 * band + out]    = b;
    }
    // The next band overwrites the noise, and the vertical blur reads every band.
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= (int)width || y >= (int)height) return;

  // G-blur-y, the unit field, and the mix of rule 'grain_layer_mix'.
  const int centre = (local_y + halo) * FILM_GRAIN_POST_TILE + local_x;
  float     r      = across[centre] * field->weights[0][0];
  float     g      = across[band + centre] * field->weights[1][0];
  float     b      = across[2 * band + centre] * field->weights[2][0];
  for (int tap = 1; tap <= halo; ++tap) {
    const int above = centre - tap * FILM_GRAIN_POST_TILE;
    const int below = centre + tap * FILM_GRAIN_POST_TILE;
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
  const FilmGrainNoise3     grain = FilmGrainMix(mix, unit);

  const FilmGrainPostParams post = *(__global const FilmGrainPostParams*)(data + post_offset);
  const float4 pixel = FilmGrainPostRead(src_image, src_buffer, src_is_buffer, x, y, width);
  FilmGrainPostRgb code;
  code.r                        = pixel.x;
  code.g                        = pixel.y;
  code.b                        = pixel.z;
  const FilmGrainPostRgb result = FilmGrainPostPixel(post, code, grain);
  const float4           output = (float4)(result.r, result.g, result.b, pixel.w);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)y * width + (uint)x] = output;
  } else {
    write_imagef(dst_image, (int2)(x, y), output);
  }
}
