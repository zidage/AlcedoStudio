//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct CameraColorGpuParams {
  float camera_to_ap1[9];
  float pad[3];
};

static inline float AcesccEncode(float value) {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (log2(value) + kA) / kB;
}

kernel void camera_color_acescc(texture2d<float, access::read> src [[texture(0)]],
                                texture2d<float, access::write> dst [[texture(1)]],
                                constant CameraColorGpuParams& camera [[buffer(0)]],
                                uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= src.get_width() || gid.y >= src.get_height()) {
    return;
  }
  const float4 source = src.read(gid);
  const float  x =
      camera.camera_to_ap1[0] * source.x + camera.camera_to_ap1[1] * source.y +
      camera.camera_to_ap1[2] * source.z;
  const float y =
      camera.camera_to_ap1[3] * source.x + camera.camera_to_ap1[4] * source.y +
      camera.camera_to_ap1[5] * source.z;
  const float z =
      camera.camera_to_ap1[6] * source.x + camera.camera_to_ap1[7] * source.y +
      camera.camera_to_ap1[8] * source.z;
  dst.write(float4(AcesccEncode(x), AcesccEncode(y), AcesccEncode(z), source.w), gid);
}
