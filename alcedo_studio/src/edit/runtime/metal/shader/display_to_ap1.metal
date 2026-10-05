//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
//  Raster input conversion to the ACEScc AP1 working space. CMake compiles this shader with
//  -fno-fast-math: the ACES 2.0 inverse is checked against OpenColorIO within 1e-3.

#include <metal_stdlib>

using namespace metal;
#include "../../../../include/edit/runtime/display_to_ap1_math.h"
#include "../../../../include/edit/runtime/raster_linearize_math.h"

/// Linearize tightly packed RGBA host-format pixels (format: 2 U8, 3 U16, 1 F32) into F32 RGBA.
kernel void linearize_raster_rgba(device const uchar*             src [[buffer(0)]],
                                  constant uint&                  format [[buffer(1)]],
                                  device const float*             params [[buffer(2)]],
                                  texture2d<float, access::write> dst [[texture(0)]],
                                  uint2                           gid [[thread_position_in_grid]]) {
  if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) {
    return;
  }
  const uint index = gid.y * dst.get_width() + gid.x;
  float      r, g, b;
  if (format == 2u) {
    device const uchar* p = src + index * 4u;
    r = float(p[0]);
    g = float(p[1]);
    b = float(p[2]);
  } else if (format == 3u) {
    device const ushort* p = reinterpret_cast<device const ushort*>(src) + index * 4u;
    r = float(p[0]);
    g = float(p[1]);
    b = float(p[2]);
  } else {
    device const float* p = reinterpret_cast<device const float*>(src) + index * 4u;
    r = p[0];
    g = p[1];
    b = p[2];
  }
  const RlRgb rgb = RlLinearize(r, g, b, params);
  dst.write(float4(rgb.r, rgb.g, rgb.b, 1.0f), gid);
}

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
