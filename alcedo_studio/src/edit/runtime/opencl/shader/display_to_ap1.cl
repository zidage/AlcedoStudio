// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Raster input: LinearizeRaster and the conversion to the ACEScc AP1 working space. The program
// source starts with include/edit/runtime/raster_linearize_math.h and display_to_ap1_math.h (see
// opencl_gpu_dag_programs.cpp).

/// Linearize tightly packed RGBA host-format pixels (format: 2 U8, 3 U16, 1 F32) into F32 RGBA.
__kernel void linearize_raster_rgba(__global const uchar* src, uint src_offset_bytes, uint format,
                                    uint width, uint height, __global const float* params,
                                    uint params_offset, __global float* dst, uint dst_offset) {
  const uint x = (uint)get_global_id(0);
  const uint y = (uint)get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }
  const uint index = y * width + x;
  float      r, g, b;
  if (format == 2u) {
    __global const uchar* p = src + src_offset_bytes + index * 4u;
    r                       = (float)p[0];
    g                       = (float)p[1];
    b                       = (float)p[2];
  } else if (format == 3u) {
    __global const ushort* p = (__global const ushort*)(src + src_offset_bytes) + index * 4u;
    r                        = (float)p[0];
    g                        = (float)p[1];
    b                        = (float)p[2];
  } else {
    __global const float* p = (__global const float*)(src + src_offset_bytes) + index * 4u;
    r                       = p[0];
    g                       = p[1];
    b                       = p[2];
  }
  const RlRgb     rgb = RlLinearize(r, g, b, params + params_offset);
  __global float* out = dst + dst_offset + index * 4u;
  out[0]              = rgb.r;
  out[1]              = rgb.g;
  out[2]              = rgb.b;
  out[3]              = 1.0f;
}

__kernel void display_to_ap1_acescc(__read_only image2d_t src, __write_only image2d_t dst,
                                    __global const float* params_base, uint params_offset) {
  __global const float* params = params_base + params_offset;
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
                                    __global const float* params_base, uint params_offset) {
  __global const float* params = params_base + params_offset;
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
