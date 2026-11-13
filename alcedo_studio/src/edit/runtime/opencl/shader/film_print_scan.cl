//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FP6 of print material Negative Scan on OpenCL 1.2, in the DRT program (issue 664): the scan
// density T of the scanner table, the point form of film_print_math.h to linear AP1, then the
// display transform of drt.cl (DrtDisplayPixel), with the same arithmetic as the kernels of method
// OpenDRT. The program loads drt.cl, film_print_math.h, film_grain_field.h, film_grain_tile.h, and
// film_grain_negative.h before this file; the last three give the form the grain of the negative
// (film_print_scan_display_grain, phase G6 of the film grain plan).
// opencl_film_print_pass.cpp records it after FP5 of film_print.cl.

// A size that differs from the host stops the build of this program, as in film_print.cl.
typedef char FilmScanParamsSizeCheck[sizeof(FilmKernelParams) == 416 ? 1 : -1];
typedef char FilmScanTermsSizeCheck[sizeof(FilmKernelNegativeScan) == 48 ? 1 : -1];
typedef char       FilmScanGrainFieldSizeCheck[sizeof(FilmGrainFieldParams) == 388 ? 1 : -1];
typedef char       FilmScanGrainNegativeSizeCheck[sizeof(FilmGrainNegativeParams) == 616 ? 1 : -1];

/// FP6 of Negative Scan at pixel @p gid with the record @p record. See film_print_scan_display.
static inline void FilmScanDisplayPixel(
    FilmKernelRgb record, int2 gid, uint width, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, __global const float* scanner_table,
    __global const FilmKernelParams* params, __global const FilmKernelNegativeScan* terms,
    __global const OpenClToOutputParams* drt, int linear_output) {
  const uint          index  = (uint)gid.y * width + (uint)gid.x;
  const FilmKernelRgb t      = FilmKernelReadRecordTable(params, scanner_table, record);
  const FilmKernelRgb linear = FilmKernelNegativeScanPixel(t, terms);
  const float         alpha =
      scene_is_buffer != 0 ? scene_buffer[index].w : read_imagef(scene_image, kNearestClamp, gid).w;
  float4 output = (float4)(linear.r, linear.g, linear.b, alpha);
  if (linear_output == 0) {
    const float3 encoded = DrtDisplayPixel((float3)(linear.r, linear.g, linear.b), drt);
    output               = (float4)(encoded.x, encoded.y, encoded.z, alpha);
  }
  if (dst_is_buffer != 0) {
    dst_buffer[index] = output;
  } else {
    write_imagef(dst_image, gid, output);
  }
}

static inline FilmKernelRgb FilmScanLoad(__global const float* amounts, uint amounts_offset,
                                         uint index) {
  __global const float* texel = amounts + amounts_offset + 3U * index;
  return FilmKernelMakeRgb(texel[0], texel[1], texel[2]);
}

/// @p amounts holds the dye amounts after FP5, 3 floats per pixel from @p amounts_offset.
/// @p film_data holds the FilmKernelParams at @p film_params_offset, and @p terms_data the
/// FilmKernelNegativeScan of this render at @p terms_offset: the terms follow the auto colour of
/// the prescan, so they have their own upload. With @p linear_output, the kernel writes the linear
/// AP1 of the form, for a test of that form.
__kernel void film_print_scan_display(
    __global const float* amounts, uint amounts_offset, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, uint width, uint height,
    __global const float* scanner_table, __global const uchar* film_data, uint film_params_offset,
    __global const uchar* terms_data, uint terms_offset, __global const uchar* drt_data,
    uint drt_params_offset, int linear_output) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) {
    return;
  }
  FilmScanDisplayPixel(
      FilmScanLoad(amounts, amounts_offset, (uint)gid.y * width + (uint)gid.x), gid, width,
      scene_image, scene_buffer, scene_is_buffer, dst_image, dst_buffer, dst_is_buffer,
      scanner_table, (__global const FilmKernelParams*)(film_data + film_params_offset),
      (__global const FilmKernelNegativeScan*)(terms_data + terms_offset),
      (__global const OpenClToOutputParams*)(drt_data + drt_params_offset), linear_output);
}

/// Phase G6: FP6 of Negative Scan with the grain of the negative, in the steps of
/// film_grain_tile.h, as film_print_display_grain of film_print.cl. The grain goes into the record
/// of each pixel before the read of the scanner table. @p grain_data holds the FilmGrainFieldParams
/// at @p field_offset and the FilmGrainNegativeParams at @p negative_offset; the work-group copies
/// them into local memory. The other arguments are those of film_print_scan_display.
__kernel __attribute__((reqd_work_group_size(FILM_GRAIN_TILE, FILM_GRAIN_TILE, 1))) void
film_print_scan_display_grain(
    __global const float* amounts, uint amounts_offset, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, uint width, uint height,
    __global const float* scanner_table, __global const uchar* film_data, uint film_params_offset,
    __global const uchar* terms_data, uint terms_offset, __global const uchar* drt_data,
    uint drt_params_offset, int linear_output, __global const uchar* grain_data, uint field_offset,
    uint negative_offset, __local float* shared) {
  const int                       local_x = (int)get_local_id(0);
  const int                       local_y = (int)get_local_id(1);
  const int                       lane    = local_y * FILM_GRAIN_TILE + local_x;
  __local FilmGrainFieldParams    field;
  __local FilmGrainNegativeParams negative;
  FilmGrainTileCopyWords(grain_data + field_offset, &field, sizeof(FilmGrainFieldParams), lane);
  FilmGrainTileCopyWords(grain_data + negative_offset, &negative, sizeof(FilmGrainNegativeParams),
                         lane);
  // Every lane reads these before the first barrier, so they come from the upload.
  __global const FilmGrainFieldParams* upload =
      (__global const FilmGrainFieldParams*)(grain_data + field_offset);
  const int halo = upload->halo;
  FilmGrainTileAcross(&field, upload->noise, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS,
                      (int)get_group_id(0), (int)get_group_id(1), lane, shared);

  const int x = (int)get_group_id(0) * FILM_GRAIN_TILE + local_x;
  if (x >= (int)width) return;
  __global const FilmKernelParams* params =
      (__global const FilmKernelParams*)(film_data + film_params_offset);
  __global const FilmKernelNegativeScan* terms =
      (__global const FilmKernelNegativeScan*)(terms_data + terms_offset);
  __global const OpenClToOutputParams* drt =
      (__global const OpenClToOutputParams*)(drt_data + drt_params_offset);
  for (int half_index = 0; half_index < FILM_GRAIN_NEGATIVE_TILE_ROWS / FILM_GRAIN_TILE;
       ++half_index) {
    const int tile_row = local_y + half_index * FILM_GRAIN_TILE;
    const int y        = (int)get_group_id(1) * FILM_GRAIN_NEGATIVE_TILE_ROWS + tile_row;
    if (y >= (int)height) return;
    const FilmGrainNoise3 grain =
        FilmGrainTileGrain(&field, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS, tile_row, local_x, shared);
    const FilmKernelRgb  a = FilmScanLoad(amounts, amounts_offset, (uint)y * width + (uint)x);
    FilmGrainNegativeRgb value;
    value.r                           = a.r;
    value.g                           = a.g;
    value.b                           = a.b;
    const FilmGrainNegativeRgb result = FilmGrainNegativeRecord(&negative, value, grain);
    FilmScanDisplayPixel(FilmKernelMakeRgb(result.r, result.g, result.b), (int2)(x, y), width,
                         scene_image, scene_buffer, scene_is_buffer, dst_image, dst_buffer,
                         dst_is_buffer, scanner_table, params, terms, drt, linear_output);
  }
}
