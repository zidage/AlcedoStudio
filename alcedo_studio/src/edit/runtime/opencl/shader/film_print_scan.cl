//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FP6 of print material Negative Scan on OpenCL 1.2, in the DRT program (issue 664): the scan
// density T of the scanner table, the point form of film_print_math.h to linear AP1, then the
// display transform of drt.cl (DrtDisplayPixel), with the same arithmetic as the kernels of method
// OpenDRT. The program loads drt.cl and film_print_math.h before this file.
// opencl_film_print_pass.cpp records it after FP5 of film_print.cl.

// A size that differs from the host stops the build of this program, as in film_print.cl.
typedef char FilmScanParamsSizeCheck[sizeof(FilmKernelParams) == 416 ? 1 : -1];
typedef char FilmScanTermsSizeCheck[sizeof(FilmKernelNegativeScan) == 48 ? 1 : -1];

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
  const uint index = (uint)gid.y * width + (uint)gid.x;
  __global const FilmKernelParams* params =
      (__global const FilmKernelParams*)(film_data + film_params_offset);
  __global const FilmKernelNegativeScan* terms =
      (__global const FilmKernelNegativeScan*)(terms_data + terms_offset);
  __global const float* texel = amounts + amounts_offset + 3U * index;
  const FilmKernelRgb   t     = FilmKernelReadRecordTable(
      params, scanner_table, FilmKernelMakeRgb(texel[0], texel[1], texel[2]));
  const FilmKernelRgb linear = FilmKernelNegativeScanPixel(t, terms);
  const float         alpha  = scene_is_buffer != 0 ? scene_buffer[index].w
                                                    : read_imagef(scene_image, kNearestClamp, gid).w;
  float4              output = (float4)(linear.r, linear.g, linear.b, alpha);
  if (linear_output == 0) {
    __global const OpenClToOutputParams* drt =
        (__global const OpenClToOutputParams*)(drt_data + drt_params_offset);
    const float3 encoded = DrtDisplayPixel((float3)(linear.r, linear.g, linear.b), drt);
    output               = (float4)(encoded.x, encoded.y, encoded.z, alpha);
  }
  if (dst_is_buffer != 0) {
    dst_buffer[index] = output;
  } else {
    write_imagef(dst_image, gid, output);
  }
}
