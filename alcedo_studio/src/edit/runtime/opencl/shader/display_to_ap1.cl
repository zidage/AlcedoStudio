// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Raster input conversion to the ACEScc AP1 working space. The program source starts with
// include/edit/runtime/display_to_ap1_math.h (see opencl_gpu_dag_programs.cpp).

__kernel void display_to_ap1_acescc(__read_only image2d_t src, __write_only image2d_t dst,
                                    __global const float* params) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(src);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  const sampler_t nearest =
      CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
  const float4    source = read_imagef(src, nearest, gid);
  const D2aFloat3 result = D2aSourceToAcesccAp1(D2aMake3(source.x, source.y, source.z), params);
  write_imagef(dst, gid, (float4)(result.x, result.y, result.z, source.w));
}

__kernel void display_to_ap0_linear(__read_only image2d_t src, __write_only image2d_t dst,
                                    __global const float* params) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(src);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  const sampler_t nearest =
      CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
  const float4    source = read_imagef(src, nearest, gid);
  const D2aFloat3 result = D2aDisplayToAp0(D2aMake3(source.x, source.y, source.z), params);
  write_imagef(dst, gid, (float4)(result.x, result.y, result.z, source.w));
}
