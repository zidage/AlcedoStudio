//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group display_to_ap1 (plan #1036, phase VK7): the raster input kernels of
// opencl_shader/display_to_ap1.cl. linearize_raster_rgba reads and writes buffers by device address;
// display_to_ap1_acescc and display_to_ap0_linear read and write RGBA32F storage images. The names,
// the bindings, and the push constant layouts are in
// include/vulkan/vulkan_display_to_ap1_kernels.hpp.

#include "color/color_encoding_math.h"
#include "edit/runtime/raster_linearize_math.h"
#include "edit/runtime/aces2_reference_math.h"
#include "edit/runtime/display_to_ap1_math.h"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

// The HostPixelFormat values of the samples (prepared_raw_input.hpp).
static const uint kFormatF32Rgba = 1u;
static const uint kFormatU8Rgba  = 2u;
static const uint kFormatU16Rgba = 3u;

// LinearizeRasterPushConstants: 40 bytes.
struct LinearizeRasterPushConstants {
  uint64_t source;       // width * height RGBA samples of `format`, 4-byte aligned
  float*   parameters;   // the PackRasterLinearize block
  float*   destination;  // width * height RGBA32F values
  uint     format;
  uint     width;
  uint     height;
  uint     reserved;     // pads the block to the 8-byte struct alignment
};

// DisplayToAp1PushConstants: 16 bytes.
struct DisplayToAp1PushConstants {
  float* parameters;  // the DisplayToAp1 block (ResolveDisplayToAp1Block)
  uint   width;
  uint   height;
};

// The 32-bit word at `address` (a multiple of 4): the samples need no 8-bit or 16-bit storage
// feature (the LoadU16 rule of raw_core.slang, phase VK4).
static uint LoadWord(uint64_t address) { return *((uint*)address); }

// Code values of each channel as floats (integer codes for U8 and U16), then RlLinearize. Alpha is
// 1.
[shader("compute")]
[numthreads(16, 16, 1)]
void linearize_raster_rgba(uint3 id: SV_DispatchThreadID,
                           uniform LinearizeRasterPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const uint index = id.y * push.width + id.x;
  float      r     = 0.0f;
  float      g     = 0.0f;
  float      b     = 0.0f;
  if (push.format == kFormatU8Rgba) {
    const uint word = LoadWord(push.source + uint64_t(index) * 4);
    r               = float(word & 0xFFu);
    g               = float((word >> 8) & 0xFFu);
    b               = float((word >> 16) & 0xFFu);
  } else if (push.format == kFormatU16Rgba) {
    const uint64_t address = push.source + uint64_t(index) * 8;
    const uint     rg      = LoadWord(address);
    const uint     ba      = LoadWord(address + 4);
    r                      = float(rg & 0xFFFFu);
    g                      = float(rg >> 16);
    b                      = float(ba & 0xFFFFu);
  } else {
    float* p = (float*)push.source + index * 4;
    r        = p[0];
    g        = p[1];
    b        = p[2];
  }
  const RlRgb rgb = RlLinearize(r, g, b, push.parameters);
  float*      out = push.destination + index * 4;
  out[0]          = rgb.r;
  out[1]          = rgb.g;
  out[2]          = rgb.b;
  out[3]          = 1.0f;
}

// Source RGB to ACEScc AP1 (the develop.image encoding). Alpha is kept.
[shader("compute")]
[numthreads(16, 16, 1)]
void display_to_ap1_acescc(uint3 id: SV_DispatchThreadID, uniform DisplayToAp1PushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const float4    value  = source[id.xy];
  const A2rFloat3 result = D2aSourceToAcesccAp1(A2rMake3(value.x, value.y, value.z), push.parameters);
  destination[id.xy]     = float4(result.x, result.y, result.z, value.w);
}

// Display RGB to linear AP0 by the ACES 2.0 inverse alone: the OpenColorIO check of the tests.
[shader("compute")]
[numthreads(16, 16, 1)]
void display_to_ap0_linear(uint3 id: SV_DispatchThreadID, uniform DisplayToAp1PushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const float4    value  = source[id.xy];
  const A2rFloat3 result = A2rDisplayToAp0(A2rMake3(value.x, value.y, value.z), push.parameters);
  destination[id.xy]     = float4(result.x, result.y, result.z, value.w);
}
