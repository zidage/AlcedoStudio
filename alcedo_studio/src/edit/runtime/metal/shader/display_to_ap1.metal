//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
//  Raster input conversion to the ACEScc AP1 working space. CMake compiles this shader with
//  -fno-fast-math: the ACES 2.0 inverse is checked against OpenColorIO within 1e-3.

#include <metal_stdlib>

using namespace metal;
#include "../../../../include/edit/runtime/display_to_ap1_math.h"

kernel void display_to_ap1_acescc(texture2d<float, access::read>  src [[texture(0)]],
                                  texture2d<float, access::write> dst [[texture(1)]],
                                  device const float*             params [[buffer(0)]],
                                  uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= src.get_width() || gid.y >= src.get_height()) {
    return;
  }
  const float4    source = src.read(gid);
  const D2aFloat3 result = D2aSourceToAcesccAp1(D2aMake3(source.x, source.y, source.z), params);
  dst.write(float4(result.x, result.y, result.z, source.w), gid);
}

kernel void display_to_ap0_linear(texture2d<float, access::read>  src [[texture(0)]],
                                  texture2d<float, access::write> dst [[texture(1)]],
                                  device const float*             params [[buffer(0)]],
                                  uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= src.get_width() || gid.y >= src.get_height()) {
    return;
  }
  const float4    source = src.read(gid);
  const D2aFloat3 result = D2aDisplayToAp0(D2aMake3(source.x, source.y, source.z), params);
  dst.write(float4(result.x, result.y, result.z, source.w), gid);
}
