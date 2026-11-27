//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Crop and bilinear resize of RGBA32F images: the kernel opencl_geometry_crop_resize_linear of
// src/opencl/opencl_shader/geometry_utils.cl, with storage images in place of row-major buffers
// (vulkan_compute_backend_plan.md, phase VK3). The name, the bindings, and the push constant
// layout are in include/vulkan/vulkan_geometry_kernels.hpp.

// CropResizePushConstants: 40 bytes, scalar layout.
struct CropResizePushConstants {
  int   source_width;
  int   source_height;
  int   crop_x;
  int   crop_y;
  int   crop_width;
  int   crop_height;
  int   destination_width;
  int   destination_height;
  float scale_x;  // crop_width / destination_width
  float scale_y;  // crop_height / destination_height
};

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

// The texel at (x, y), or 0 outside the source.
static float4 ReadTexel(int x, int y, int width, int height) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return float4(0.0f);
  }
  return source[uint2(uint(x), uint(y))];
}

static float4 BilinearSample(float sx, float sy, int width, int height) {
  const int    x0  = int(floor(sx));
  const int    y0  = int(floor(sy));
  const int    x1  = x0 + 1;
  const int    y1  = y0 + 1;
  const float  fx  = sx - float(x0);
  const float  fy  = sy - float(y0);
  const float  w00 = (1.0f - fx) * (1.0f - fy);
  const float  w10 = fx * (1.0f - fy);
  const float  w01 = (1.0f - fx) * fy;
  const float  w11 = fx * fy;
  const float4 p00 = ReadTexel(x0, y0, width, height);
  const float4 p10 = ReadTexel(x1, y0, width, height);
  const float4 p01 = ReadTexel(x0, y1, width, height);
  const float4 p11 = ReadTexel(x1, y1, width, height);
  return p00 * w00 + p10 * w10 + p01 * w01 + p11 * w11;
}

// Bilinear sample at the center of each destination texel, mapped into the crop.
[shader("compute")]
[numthreads(16, 16, 1)]
void geometry_crop_resize_linear(uint3 id: SV_DispatchThreadID,
                                 uniform CropResizePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.destination_width || y >= push.destination_height) {
    return;
  }
  const float sx = float(push.crop_x) + (float(x) + 0.5f) * push.scale_x - 0.5f;
  const float sy = float(push.crop_y) + (float(y) + 0.5f) * push.scale_y - 0.5f;
  destination[id.xy] = BilinearSample(sx, sy, push.source_width, push.source_height);
}
