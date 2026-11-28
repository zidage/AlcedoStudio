//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group drt (plan #1036, phase VK12): the display transform kernel of
// opencl/shader/drt.cl that the DRT pass dispatches (drt_display_scene_rgba32f). The name, the
// bindings, and the push constant layout are in include/vulkan/vulkan_drt_kernels.hpp.
//
// The source is the linear AP1 output of the DiffusionFilter pass: a cached RGBA32F storage image
// or a scene-work member (an RGBA32F row-major buffer). The destination is the RGBA32F display
// output image or the free scene-work member. A kernel reads or writes the buffer when its address
// is not 0, else the image of its binding.

#include "edit/runtime/vulkan/shader/drt_display.slangh"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination_image;

// DrtDisplayPushConstants: 32 bytes.
struct DrtDisplayPushConstants {
  uint64_t source;       // float4 row-major, or 0: source_image
  uint64_t destination;  // float4 row-major, or 0: destination_image
  uint64_t parameters;   // the block of vulkan_drt_layout.h (a parameter arena slot)
  uint     width;
  uint     height;
};

static float4* Float4At(uint64_t address) { return (float4*)address; }

[shader("compute")]
[numthreads(16, 16, 1)]
void drt_display_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                               uniform DrtDisplayPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const uint   index  = id.y * push.width + id.x;
  const float4 source = push.source != 0u ? Float4At(push.source)[index] : source_image[id.xy];
  // Input is linear AP1; the DiffusionFilter pass already decoded ACEScc.
  const float3 encoded = DrtDisplayPixel(source.xyz, push.parameters);
  const float4 result  = float4(encoded, source.w);
  if (push.destination != 0u) {
    Float4At(push.destination)[index] = result;
  } else {
    destination_image[id.xy] = result;
  }
}
