//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// FP6 of print material Lab Scan on OpenCL 1.2, in the DRT program: the scan form of
// film_print_math.h to linear AP1, then the display transform of drt.cl (DrtDisplayPixel), with the
// same arithmetic as the kernels of method OpenDRT. The program loads film_print_math.h before this
// file, and drt.cl before it. opencl_film_print_pass.cpp records it after FP5 of film_print.cl.

// A size that differs from the host stops the build of this program, as in film_print.cl.
typedef char FilmScanParamsSizeCheck[sizeof(FilmKernelParams) == 628 ? 1 : -1];

/// @p amounts holds the dye amounts after FP5, 3 floats per pixel from @p amounts_offset. With
/// @p scene_output, the kernel writes the linear AP1 of the scan form, for a test of that form.
__kernel void film_print_scan_display(
    __global const float* amounts, uint amounts_offset, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, uint width, uint height,
    __global const float* density, __global const uchar* film_data, uint film_params_offset,
    __global const uchar* drt_data, uint drt_params_offset, int scene_output) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) {
    return;
  }
  const uint index = (uint)gid.y * width + (uint)gid.x;
  __global const FilmKernelParams* params =
      (__global const FilmKernelParams*)(film_data + film_params_offset);
  __global const float* texel  = amounts + amounts_offset + 3U * index;
  const FilmKernelRgb   ap1    = FilmKernelScanSceneAp1(
      params, density, FilmKernelMakeRgb(texel[0], texel[1], texel[2]));
  const float           alpha  = scene_is_buffer != 0
                                     ? scene_buffer[index].w
                                     : read_imagef(scene_image, kNearestClamp, gid).w;
  float4                output = (float4)(ap1.r, ap1.g, ap1.b, alpha);
  if (scene_output == 0) {
    __global const OpenClToOutputParams* drt =
        (__global const OpenClToOutputParams*)(drt_data + drt_params_offset);
    const float3 encoded = DrtDisplayPixel((float3)(ap1.r, ap1.g, ap1.b), drt);
    output               = (float4)(encoded.x, encoded.y, encoded.z, alpha);
  }
  if (dst_is_buffer != 0) {
    dst_buffer[index] = output;
  } else {
    write_imagef(dst_image, gid, output);
  }
}
