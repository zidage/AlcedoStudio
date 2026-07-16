//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct BoundaryParams {
  uint  element_count;
  float addend;
};

kernel void demosaicnet_boundary_prepare(device const float* src [[buffer(0)]],
                                         device float*       dst [[buffer(1)]],
                                         constant BoundaryParams& params [[buffer(2)]],
                                         uint                 gid [[thread_position_in_grid]]) {
  if (gid >= params.element_count) {
    return;
  }
  dst[gid] = src[gid] + params.addend;
}

kernel void demosaicnet_boundary_finish(device const float* src [[buffer(0)]],
                                        device float*       dst [[buffer(1)]],
                                        constant BoundaryParams& params [[buffer(2)]],
                                        uint                 gid [[thread_position_in_grid]]) {
  if (gid >= params.element_count) {
    return;
  }
  dst[gid] = src[gid] + params.addend;
}
