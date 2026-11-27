//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group raw_pack: the Slang form of pack_planes_crop_inverse_orient and
// copy_rgba_crop_inverse_orient of opencl_shader/cvt_ref_space.cl (plan phase VK4). The source is
// at device addresses; the destination is the RGBA32F storage image of the graph (binding 0). The
// names, the binding, and the push constant layouts are in
// include/vulkan/vulkan_raw_pack_kernels.hpp.

#include "raw_pack.slangh"

// PackPlanesPushConstants: 72 bytes.
struct PackPlanesPushConstants {
  float*           r;
  float*           g;
  float*           b;
  PackOrientParams params;
};

// CopyRgbaPushConstants: 56 bytes.
struct CopyRgbaPushConstants {
  float*           source;  // RGBA values, row-major with params.src_stride texels in a row
  PackOrientParams params;
};

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

[shader("compute")]
[numthreads(16, 16, 1)]
void pack_planes_crop_inverse_orient(uint3 id: SV_DispatchThreadID,
                                     uniform PackPlanesPushConstants push) {
  const PackOrientParams params = push.params;
  if (id.x >= params.src_width || id.y >= params.src_height) {
    return;
  }
  const uint src = (params.src_y + id.y) * params.plane_stride + (params.src_x + id.x);
  destination[OrientedCoord(id.x, id.y, params)] =
      float4(push.r[src] * params.scale_r, push.g[src] * params.scale_g,
             push.b[src] * params.scale_b, 1.0f);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void copy_rgba_crop_inverse_orient(uint3 id: SV_DispatchThreadID,
                                   uniform CopyRgbaPushConstants push) {
  const PackOrientParams params = push.params;
  if (id.x >= params.src_width || id.y >= params.src_height) {
    return;
  }
  const uint base = ((params.src_y + id.y) * params.src_stride + (params.src_x + id.x)) * 4;
  destination[OrientedCoord(id.x, id.y, params)] =
      float4(push.source[base] * params.scale_r, push.source[base + 1] * params.scale_g,
             push.source[base + 2] * params.scale_b, push.source[base + 3]);
}
