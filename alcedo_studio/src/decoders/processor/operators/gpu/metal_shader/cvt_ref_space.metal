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
