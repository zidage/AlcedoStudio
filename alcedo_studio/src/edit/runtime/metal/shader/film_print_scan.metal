//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FP6 of print material Negative Scan on Metal, in the DRT AIR file (issue 662): the scan density T
// of the scanner table, the point form of film_print_math.h to linear AP1, then the display
// transform of drt.metal (DrtDisplayPixel), with the same arithmetic as `drt_display` of method
// OpenDRT. drt.metal includes this file after its display transform functions.
// metal_film_print_pass.mm records it after FP5 of film_print.metal. The grain headers give the
// form the grain of the negative (film_print_scan_display_grain, phase G6 of the film grain plan).

#include "../../../../include/edit/runtime/film/film_grain_field.h"
#include "../../../../include/edit/runtime/film/film_grain_negative.h"
#include "../../../../include/edit/runtime/film/film_grain_tile.h"
#include "../../../../include/edit/runtime/film/film_print_math.h"

// A size that differs from the host stops the build of this library, as in film_print.metal.
static_assert(sizeof(FilmKernelParams) == 416, "FilmKernelParams differs from the host");
static_assert(sizeof(FilmKernelNegativeScan) == 48, "FilmKernelNegativeScan differs from the host");
static_assert(sizeof(FilmGrainFieldParams) == 388, "FilmGrainFieldParams differs from the host");
static_assert(sizeof(FilmGrainNegativeParams) == 616,
              "FilmGrainNegativeParams differs from the host");

/// `FilmSize` of film_print.metal.
struct FilmScanSize {
  int width;
  int height;
};

/// FP6 of Negative Scan at pixel @p gid with the record @p record. See film_print_scan_display.
static inline void FilmScanDisplayPixel(
    FilmKernelRgb record, uint2 gid, const device float* scanner_table,
    constant FilmKernelParams& params, constant FilmKernelNegativeScan& terms,
    constant MetalToOutputParams& drt, int linear_output, const device uint* alpha_mark, uint stamp,
    texture2d<float, access::read> scene, texture2d<float, access::write> display) {
  const FilmKernelRgb t      = FilmKernelReadRecordTable(&params, scanner_table, record);
  const FilmKernelRgb linear = FilmKernelNegativeScanPixel(t, &terms);
  const float         alpha  = alpha_mark[0] == stamp ? scene.read(gid).w : 1.0f;
  const float3        value  = float3(linear.r, linear.g, linear.b);
  display.write(float4(linear_output != 0 ? value : DrtDisplayPixel(value, drt), alpha), gid);
}

/// @p amounts holds the dye amounts after FP5, packed_float3 per pixel. @p terms follow the auto
/// colour of the prescan, so they are an argument of this render. @p drt is the transform of
/// method OpenDRT that the resolver gives Negative Scan. With @p linear_output not 0, the kernel
/// writes the linear AP1 of the form, for a test of that form. The alpha mark is that of
/// `film_print_display`: the scene is read only when FP1 marked an alpha other than 1.
kernel void film_print_scan_display(const device packed_float3*      amounts [[buffer(0)]],
                                    const device float*              scanner_table [[buffer(1)]],
                                    constant FilmKernelParams&       params [[buffer(2)]],
                                    constant FilmKernelNegativeScan& terms [[buffer(3)]],
                                    constant MetalToOutputParams&    drt [[buffer(4)]],
                                    constant FilmScanSize&           size [[buffer(5)]],
                                    constant int&                    linear_output [[buffer(6)]],
                                    const device uint*               alpha_mark [[buffer(7)]],
                                    constant uint&                   stamp [[buffer(8)]],
                                    texture2d<float, access::read>   scene [[texture(0)]],
                                    texture2d<float, access::write>  display [[texture(1)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const float3 a = float3(amounts[gid.y * uint(size.width) + gid.x]);
  FilmScanDisplayPixel(FilmKernelMakeRgb(a.x, a.y, a.z), gid, scanner_table, params, terms, drt,
                       linear_output, alpha_mark, stamp, scene, display);
}

/// Phase G6: FP6 of Negative Scan with the grain of the negative, in the steps of
/// film_grain_tile.h, as film_print_display_grain of film_print.metal. The grain goes into the
/// record of each pixel before the read of the scanner table. The other arguments are those of
/// film_print_scan_display.
kernel void film_print_scan_display_grain(
    const device packed_float3*      amounts [[buffer(0)]],
    const device float*              scanner_table [[buffer(1)]],
    constant FilmKernelParams&       params [[buffer(2)]],
    constant FilmKernelNegativeScan& terms [[buffer(3)]],
    constant MetalToOutputParams& drt [[buffer(4)]], constant FilmScanSize& size [[buffer(5)]],
    constant int& linear_output [[buffer(6)]], const device uint* alpha_mark [[buffer(7)]],
    constant uint& stamp [[buffer(8)]], constant FilmGrainFieldParams& field [[buffer(9)]],
    constant FilmGrainNegativeParams& negative [[buffer(10)]],
    texture2d<float, access::read>    scene [[texture(0)]],
    texture2d<float, access::write>   display [[texture(1)]],
    threadgroup float* shared [[threadgroup(0)]], uint2 group [[threadgroup_position_in_grid]],
    uint2 lane2 [[thread_position_in_threadgroup]]) {
  const int halo    = field.halo;
  const int local_x = int(lane2.x);
  const int local_y = int(lane2.y);
  FilmGrainTileAcross(&field, field.noise, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS, int(group.x),
                      int(group.y), local_y * FILM_GRAIN_TILE + local_x, shared);

  const int x = int(group.x) * FILM_GRAIN_TILE + local_x;
  if (x >= size.width) return;
  for (int half_index = 0; half_index < FILM_GRAIN_NEGATIVE_TILE_ROWS / FILM_GRAIN_TILE;
       ++half_index) {
    const int tile_row = local_y + half_index * FILM_GRAIN_TILE;
    const int y        = int(group.y) * FILM_GRAIN_NEGATIVE_TILE_ROWS + tile_row;
    if (y >= size.height) return;
    const FilmGrainNoise3 grain =
        FilmGrainTileGrain(&field, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS, tile_row, local_x, shared);
    const uint2          gid = uint2(uint(x), uint(y));
    const float3         a   = float3(amounts[gid.y * uint(size.width) + gid.x]);
    FilmGrainNegativeRgb value;
    value.r                           = a.x;
    value.g                           = a.y;
    value.b                           = a.z;
    const FilmGrainNegativeRgb result = FilmGrainNegativeRecord(&negative, value, grain);
    FilmScanDisplayPixel(FilmKernelMakeRgb(result.r, result.g, result.b), gid, scanner_table,
                         params, terms, drt, linear_output, alpha_mark, stamp, scene, display);
  }
}
