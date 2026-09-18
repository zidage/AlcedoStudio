//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct InverseCamMulParams {
  float scale_r;
  float scale_g;
  float scale_b;
  float scale_a;
};

kernel void apply_inverse_cam_mul_rgba32f(texture2d<float, access::read_write> image [[texture(0)]],
                                          constant InverseCamMulParams& params [[buffer(0)]],
                                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= image.get_width() || gid.y >= image.get_height()) {
    return;
  }

  float4 rgba = image.read(gid);
  rgba.r *= params.scale_r;
  rgba.g *= params.scale_g;
  rgba.b *= params.scale_b;
  rgba.a *= params.scale_a;
  image.write(rgba, gid);
}

struct PackOrientParams {
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
};

static inline uint2 OrientedCoord(uint x, uint y, constant PackOrientParams& params) {
  switch (params.flip) {
    case 3u:
      return uint2(params.src_width - 1u - x, params.src_height - 1u - y);
    case 5u:
      return uint2(y, params.src_width - 1u - x);
    case 6u:
      return uint2(params.src_height - 1u - y, x);
    default:
      return uint2(x, y);
  }
}

kernel void pack_planes_crop_inverse_orient(texture2d<float, access::read> r [[texture(0)]],
                                            texture2d<float, access::read> g [[texture(1)]],
                                            texture2d<float, access::read> b [[texture(2)]],
                                            texture2d<float, access::write> dst [[texture(3)]],
                                            constant PackOrientParams& params [[buffer(0)]],
                                            uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.src_width || gid.y >= params.src_height) {
    return;
  }
  const uint2 src = uint2(params.src_x + gid.x, params.src_y + gid.y);
  const float4 rgba =
      float4(r.read(src).r * params.scale_r, g.read(src).r * params.scale_g,
             b.read(src).r * params.scale_b, 1.0f);
  dst.write(rgba, OrientedCoord(gid.x, gid.y, params));
}

kernel void copy_rgba_crop_inverse_orient(texture2d<float, access::read> src [[texture(0)]],
                                          texture2d<float, access::write> dst [[texture(1)]],
                                          constant PackOrientParams& params [[buffer(0)]],
                                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.src_width || gid.y >= params.src_height) {
    return;
  }
  const float4 pixel = src.read(uint2(params.src_x + gid.x, params.src_y + gid.y));
  const float4 rgba =
      float4(pixel.r * params.scale_r, pixel.g * params.scale_g, pixel.b * params.scale_b, pixel.a);
  dst.write(rgba, OrientedCoord(gid.x, gid.y, params));
}
