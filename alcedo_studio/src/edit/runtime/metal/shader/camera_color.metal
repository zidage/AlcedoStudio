//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;
#include "../../../../include/color/color_encoding_math.h"
#include "../../../../include/edit/runtime/aces_reference_gamut_compression.h"
#include "../../../../include/edit/runtime/dng_profile_gpu_math.h"

struct CameraColorGpuParams {
  float camera_to_ap1[9];
  float pad[3];
};

kernel void camera_color_acescc(texture2d<float, access::read>  src [[texture(0)]],
                                texture2d<float, access::write> dst [[texture(1)]],
                                constant CameraColorGpuParams&  camera [[buffer(0)]],
                                device const float*             dng_profile [[buffer(1)]],
                                uint2                           gid [[thread_position_in_grid]]) {
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
  const auto corrected  = DngApplyColorProfile(DngMakeRgb(x, y, z), dng_profile);
  const auto compressed = AcesReferenceGamutCompress(corrected.r, corrected.g, corrected.b);
  dst.write(float4(CeAcesccEncode(compressed.r), CeAcesccEncode(compressed.g),
                   CeAcesccEncode(compressed.b), source.w),
            gid);
}
