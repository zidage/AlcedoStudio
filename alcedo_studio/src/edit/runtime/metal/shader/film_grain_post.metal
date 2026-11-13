//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DRT/Post Film Grain on Metal (plan 'film_grain_format_and_stock_plan.md', phase G4): the
// steps of FilmGrainTileKernel of cuda_film_grain_tile.cuh with the apply step of
// film_grain_post.h, in one kernel, as film_grain_post.cl. metal_film_grain_pass.mm records it.
//
// This file has its own AIR file, which the DRT metallib links (src/metal/CMakeLists.txt). It is
// compiled with -DCE_METAL_POW_EXP2_LOG2: the power of color_encoding_math.h in the decode and in
// the tone coordinate is exp2(y log2 x), as in the OpenCL program of the grain. With precise::pow
// the kernel took 28 ms at 6000 x 4000 on an Apple M4, and 16 ms with this form.
//
// Each 16 x 16 threadgroup makes the noise of its tile and a halo of the largest blur radius, at
// the reference pixels of the render pixels, so a viewport region has the grain of the same region
// of the full frame. film_grain_tile.h has the steps and the layout of the threadgroup memory. The
// pass has no noise or blur image in device memory, so it moves only the source and the output
// pixels.

#include <metal_stdlib>
using namespace metal;

#include "../../../../include/color/color_encoding_math.h"
#include "../../../../include/edit/runtime/film/film_grain_field.h"
#include "../../../../include/edit/runtime/film/film_grain_post.h"
#include "../../../../include/edit/runtime/film/film_grain_tile.h"

// A size that differs from the host stops the build of this library.
static_assert(sizeof(FilmGrainFieldParams) == 388, "FilmGrainFieldParams differs from the host");
static_assert(sizeof(FilmGrainPostParams) == 612, "FilmGrainPostParams differs from the host");

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
  const int halo    = field.halo;
  const int local_x = int(lane2.x);
  const int local_y = int(lane2.y);
  FilmGrainTileAcross(&field, field.noise, halo, FILM_GRAIN_POST_TILE_ROWS, int(group.x),
                      int(group.y), local_y * FILM_GRAIN_TILE + local_x, shared);

  const int x = int(group.x) * FILM_GRAIN_TILE + local_x;
  const int y = int(group.y) * FILM_GRAIN_TILE + local_y;
  if (x >= int(size.width) || y >= int(size.height)) return;
  const FilmGrainNoise3 grain =
      FilmGrainTileGrain(&field, halo, FILM_GRAIN_POST_TILE_ROWS, local_y, local_x, shared);

  const uint2      position = uint2(uint(x), uint(y));
  const float4     pixel    = src.read(position);
  FilmGrainPostRgb code;
  code.r                        = pixel.x;
  code.g                        = pixel.y;
  code.b                        = pixel.z;
  const FilmGrainPostRgb result = FilmGrainPostPixel(&post, code, grain);
  dst.write(float4(result.r, result.g, result.b, pixel.w), position);
}
