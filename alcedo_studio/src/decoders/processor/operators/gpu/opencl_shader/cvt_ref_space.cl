//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL kernels for reference-space color conversion.

typedef struct {
  float scale_r;
  float scale_g;
  float scale_b;
  float scale_a;
  uint  width;
  uint  height;
  uint  stride;
} InverseCamMulParams;

__kernel void apply_inverse_cam_mul_rgba32f(global float4* buffer,
                                            InverseCamMulParams params) {
  uint x = get_global_id(0);
  uint y = get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }

  uint   idx = y * params.stride + x;
  float4 v   = buffer[idx];
  v.x *= params.scale_r;
  v.y *= params.scale_g;
  v.z *= params.scale_b;
  v.w *= params.scale_a;
  buffer[idx] = v;
}

typedef struct {
  uint  src_x;
  uint  src_y;
  uint  src_width;
  uint  src_height;
  uint  dst_width;
  uint  dst_height;
  uint  flip;
  float scale_r;
  float scale_g;
  float scale_b;
  uint  plane_stride;
  uint  src_stride;
} PackOrientParams;

static inline int2 OrientedCoord(uint x, uint y, PackOrientParams params) {
  if (params.flip == 3u) {
    return (int2)((int)(params.src_width - 1u - x), (int)(params.src_height - 1u - y));
  }
  if (params.flip == 5u) {
    return (int2)((int)y, (int)(params.src_width - 1u - x));
  }
  if (params.flip == 6u) {
    return (int2)((int)(params.src_height - 1u - y), (int)x);
  }
  return (int2)((int)x, (int)y);
}

__kernel void pack_planes_crop_inverse_orient(global const float* r, global const float* g,
                                              global const float* b, __write_only image2d_t dst,
                                              PackOrientParams params, uint r_off, uint g_off,
                                              uint b_off) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= params.src_width || y >= params.src_height) {
    return;
  }
  const uint src = (params.src_y + y) * params.plane_stride + (params.src_x + x);
  const float4 rgba =
      (float4)(r[r_off + src] * params.scale_r, g[g_off + src] * params.scale_g,
               b[b_off + src] * params.scale_b, 1.0f);
  write_imagef(dst, OrientedCoord(x, y, params), rgba);
}

__kernel void copy_rgba_crop_inverse_orient(global const float4* src, __write_only image2d_t dst,
                                            PackOrientParams params, uint src_off) {
  const uint x = get_global_id(0);
  const uint y = get_global_id(1);
  if (x >= params.src_width || y >= params.src_height) {
    return;
  }
  const uint index = src_off + (params.src_y + y) * params.src_stride + (params.src_x + x);
  const float4 pixel = src[index];
  const float4 rgba =
      (float4)(pixel.x * params.scale_r, pixel.y * params.scale_g, pixel.z * params.scale_b, pixel.w);
  write_imagef(dst, OrientedCoord(x, y, params), rgba);
}
