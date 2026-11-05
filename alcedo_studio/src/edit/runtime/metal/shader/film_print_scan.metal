//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FP6 of print material Negative Scan on Metal, in the DRT AIR file (issue 662): the scan density T
// of the scanner table, the point form of film_print_math.h to linear AP1, then the display
// transform of drt.metal (DrtDisplayPixel), with the same arithmetic as `drt_display` of method
// OpenDRT. drt.metal includes this file after its display transform functions.
// metal_film_print_pass.mm records it after FP5 of film_print.metal.

#include "../../../../include/edit/runtime/film/film_print_math.h"

// A size that differs from the host stops the build of this library, as in film_print.metal.
static_assert(sizeof(FilmKernelParams) == 416, "FilmKernelParams differs from the host");
static_assert(sizeof(FilmKernelNegativeScan) == 48, "FilmKernelNegativeScan differs from the host");

/// `FilmSize` of film_print.metal.
struct FilmScanSize {
  int width;
  int height;
};

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
  const float3        a = float3(amounts[gid.y * uint(size.width) + gid.x]);
  const FilmKernelRgb t =
      FilmKernelReadRecordTable(&params, scanner_table, FilmKernelMakeRgb(a.x, a.y, a.z));
  const FilmKernelRgb linear = FilmKernelNegativeScanPixel(t, &terms);
  const float         alpha  = alpha_mark[0] == stamp ? scene.read(gid).w : 1.0f;
  const float3        value  = float3(linear.r, linear.g, linear.b);
  display.write(float4(linear_output != 0 ? value : DrtDisplayPixel(value, drt), alpha), gid);
}
