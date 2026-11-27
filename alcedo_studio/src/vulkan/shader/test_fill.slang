//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The check kernels of the Vulkan backend. They write a known value, so a test or
// alcedo-gpu-check --vulkan can prove that the build rule, the shader module registry, and the
// compute pipeline cache give a pipeline that runs. The names, the binding number, and the push
// constant layouts are in include/vulkan/vulkan_test_fill_kernels.hpp.

// Added to each written value. Specialization constant 0.
[vk::constant_id(0)] const uint kAddend = 0;

// FillBufferPushConstants: 16 bytes, scalar layout.
struct FillBufferPushConstants {
  uint* output;  // device address of `count` uint values
  uint  count;
  uint  value;
};

// FillImagePushConstants: 24 bytes, scalar layout.
struct FillImagePushConstants {
  float4 value;
  uint   width;
  uint   height;
};

[[vk::binding(0, 0)]] RWTexture2D<float4> fill_image;

// output[i] = value + kAddend for i < count.
[shader("compute")]
[numthreads(64, 1, 1)]
void test_fill(uint3 id: SV_DispatchThreadID, uniform FillBufferPushConstants push) {
  if (id.x >= push.count) {
    return;
  }
  push.output[id.x] = push.value + kAddend;
}

// Each texel of the RGBA32F storage image at binding 0 = value.
[shader("compute")]
[numthreads(8, 8, 1)]
void test_fill_image(uint3 id: SV_DispatchThreadID, uniform FillImagePushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  fill_image[id.xy] = push.value;
}
