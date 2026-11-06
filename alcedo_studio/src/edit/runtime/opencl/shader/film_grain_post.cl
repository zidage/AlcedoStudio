//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DRT/Post Film Grain on OpenCL (plan 'film_grain_format_and_stock_plan.md', phase G4): the
// steps of FilmGrainTileKernel of cuda_film_grain_tile.cuh with the apply step of
// film_grain_post.h, in one kernel. The program lists color_encoding_math.h, film_grain_field.h,
// film_grain_tile.h, and film_grain_post.h before this file.
//
// Each 16 x 16 work-group makes the noise of its tile and a halo of the largest blur radius, at the
// reference pixels of the render pixels, so a viewport region has the grain of the same region of
// the full frame. film_grain_tile.h has the steps and the layout of the local memory.
//
// The fixed values of the render are one upload of FilmGrainPostDeviceData of
// opencl_film_grain_pass.cpp: a FilmGrainFieldParams at `field_offset` and a FilmGrainPostParams
// at `post_offset`, in bytes from `data`. They are too large for by-value arguments of OpenCL 1.2.
// Each work-group copies them into local memory (1000 bytes), so film_grain_field.h and
// film_grain_post.h read their parameters through `__local const` pointers on OpenCL.

__constant sampler_t kFilmGrainPostSampler =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

static inline float4 FilmGrainPostRead(read_only image2d_t image, __global const float4* buffer,
                                       int is_buffer, int x, int y, uint width) {
  if (is_buffer != 0) {
    return buffer[(uint)y * width + (uint)x];
  }
  return read_imagef(image, kFilmGrainPostSampler, (int2)(x, y));
}

__kernel __attribute__((reqd_work_group_size(FILM_GRAIN_TILE, FILM_GRAIN_TILE, 1))) void
film_grain_post_rgba32f(read_only image2d_t src_image, __global const float4* src_buffer,
                        int src_is_buffer, write_only image2d_t dst_image,
                        __global float4* dst_buffer, int dst_is_buffer, __global const uchar* data,
                        uint field_offset, uint post_offset, uint width, uint height,
                        __local float* shared) {
  __global const FilmGrainFieldParams* field_upload =
      (__global const FilmGrainFieldParams*)(data + field_offset);
  const int                    local_x = (int)get_local_id(0);
  const int                    local_y = (int)get_local_id(1);
  const int                    lane    = local_y * FILM_GRAIN_TILE + local_x;
  // The work-group copies the fixed values of the render once into local memory: the blur loops
  // read the weights of every tap, and the apply step reads the curves of every pixel. With these
  // reads from global memory, the pass took 7.6 ms in place of 5.7 ms at 6000 px (phase G4). The
  // first barrier of FilmGrainTileAcross publishes the copies.
  __local FilmGrainFieldParams field_local;
  __local FilmGrainPostParams  post;
  FilmGrainTileCopyWords(data + field_offset, &field_local, sizeof(FilmGrainFieldParams), lane);
  FilmGrainTileCopyWords(data + post_offset, &post, sizeof(FilmGrainPostParams), lane);
  // Every lane reads these before the first barrier, so they come from the upload.
  const int halo = field_upload->halo;
  FilmGrainTileAcross(&field_local, field_upload->noise, halo, FILM_GRAIN_POST_TILE_ROWS,
                      (int)get_group_id(0), (int)get_group_id(1), lane, shared);

  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= (int)width || y >= (int)height) return;
  const FilmGrainNoise3 grain =
      FilmGrainTileGrain(&field_local, halo, FILM_GRAIN_POST_TILE_ROWS, local_y, local_x, shared);

  const float4 pixel = FilmGrainPostRead(src_image, src_buffer, src_is_buffer, x, y, width);
  FilmGrainPostRgb code;
  code.r                        = pixel.x;
  code.g                        = pixel.y;
  code.b                        = pixel.z;
  const FilmGrainPostRgb result = FilmGrainPostPixel(&post, code, grain);
  const float4           output = (float4)(result.r, result.g, result.b, pixel.w);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)y * width + (uint)x] = output;
  } else {
    write_imagef(dst_image, (int2)(x, y), output);
  }
}
