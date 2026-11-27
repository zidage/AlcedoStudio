//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group raw_pack: the Slang form of pack_planes_crop_inverse_orient and
// copy_rgba_crop_inverse_orient of opencl_shader/cvt_ref_space.cl (plan phase VK4). The source is
// at device addresses; the destination is the RGBA32F storage image of the graph (binding 0). The
// names, the binding, and the push constant layouts are in
// include/vulkan/vulkan_raw_pack_kernels.hpp.

// PackOrientParams: 48 bytes, the struct of cvt_ref_space.cl.
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
  uint  plane_stride;
  uint  src_stride;
};

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

// The destination texel of crop texel (x, y): flip 3 turns 180 degrees, flip 5 and 6 turn 90
// degrees, every other value keeps the orientation.
static uint2 OrientedCoord(uint x, uint y, PackOrientParams params) {
  if (params.flip == 3u) {
    return uint2(params.src_width - 1u - x, params.src_height - 1u - y);
  }
  if (params.flip == 5u) {
    return uint2(y, params.src_width - 1u - x);
  }
  if (params.flip == 6u) {
    return uint2(params.src_height - 1u - y, x);
  }
  return uint2(x, y);
}

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
