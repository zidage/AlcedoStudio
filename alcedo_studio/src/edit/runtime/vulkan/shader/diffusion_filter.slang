//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group diffusion_filter (plan #1036, phase VK12): the DiffusionFilter kernels of
// opencl/shader/drt.cl, with the same arithmetic in the same order. The names, the bindings, and
// the push constant layouts are in include/vulkan/vulkan_diffusion_filter_kernels.hpp.
//
// A scene is a cached RGBA32F storage image or a scene-work member (an RGBA32F row-major buffer):
// a kernel reads or writes the buffer when its address is not 0, else the image of its binding.
// The pyramid levels and the scatter image are RGBA32F storage images. Texel `i` covers
// [i, i + 1); every read clamps to the edge (the OpenCL sampler CLK_ADDRESS_CLAMP_TO_EDGE).

#include "color/color_encoding_math.h"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> scene_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> output_image;
[[vk::binding(2, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> coarse_image;
[[vk::binding(3, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> level_image;

// DecodePushConstants: 24 bytes.
struct DecodePushConstants {
  uint64_t source;       // float4 row-major, or 0: scene_image
  uint64_t destination;  // float4 row-major, or 0: output_image
  uint     width;
  uint     height;
};

// ReducePushConstants: 64 bytes (DiffusionReduceParams of drt.cl and the source address).
struct ReducePushConstants {
  uint64_t source;  // float4 row-major, or 0: scene_image
  int      src_width;
  int      src_height;
  int      dst_width;
  int      dst_height;
  int      samples;
  float    gain;
  float    low;
  float    high;
  float    base_to_render[6];  // the first two rows of the 3 x 3 matrix
};

// DownsamplePushConstants: 16 bytes. coarse_image is the finer source level here.
struct DownsamplePushConstants {
  int src_width;
  int src_height;
  int dst_width;
  int dst_height;
};

// UpsamplePushConstants: 24 bytes.
struct UpsamplePushConstants {
  int   coarse_width;
  int   coarse_height;
  int   width;
  int   height;
  float coarse_weight;
  float level_weight;
};

// MixPushConstants: 64 bytes (DiffusionMixParams of drt.cl and the addresses).
struct MixPushConstants {
  uint64_t source;       // float4 row-major, or 0: scene_image
  uint64_t destination;  // float4 row-major, or 0: output_image
  int      width;
  int      height;
  int      scatter_width;
  int      scatter_height;
  float    scatter_fraction;
  float    transmission;
  float    render_to_base[6];  // the first two rows of the 3 x 3 matrix
};

static float4* Float4At(uint64_t address) { return (float4*)address; }

static float2 DiffusionTransform(float m0, float m1, float m2, float m3, float m4, float m5,
                                 float x, float y) {
  return float2(m0 * x + m1 * y + m2, m3 * x + m4 * y + m5);
}

static float4 DiffusionDecode(float4 value) {
  return float4(CeAcesccDecode(value.x), CeAcesccDecode(value.y), CeAcesccDecode(value.z),
                value.w);
}

static float4 SceneFetch(uint64_t buffer, int width, int height, int x, int y) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  if (buffer != 0u) {
    return Float4At(buffer)[uint(y) * uint(width) + uint(x)];
  }
  return scene_image[uint2(uint(x), uint(y))];
}

static float4 CoarseFetch(int width, int height, int x, int y) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return coarse_image[uint2(uint(x), uint(y))];
}

// Bilinear sample of coarse_image with clamp-to-edge addressing (DiffusionBilinear).
static float4 CoarseBilinear(int width, int height, float px, float py) {
  const float  fx     = px - 0.5f;
  const float  fy     = py - 0.5f;
  const float  x0     = floor(fx);
  const float  y0     = floor(fy);
  const float  ax     = fx - x0;
  const float  ay     = fy - y0;
  const int    ix     = int(x0);
  const int    iy     = int(y0);
  const float4 a      = CoarseFetch(width, height, ix, iy);
  const float4 b      = CoarseFetch(width, height, ix + 1, iy);
  const float4 c      = CoarseFetch(width, height, ix, iy + 1);
  const float4 d      = CoarseFetch(width, height, ix + 1, iy + 1);
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

// Bilinear sample of the ACEScc scene, decoded to linear AP1 per tap before interpolation.
static float4 SceneBilinearDecoded(uint64_t buffer, int width, int height, float px, float py) {
  const float  fx     = px - 0.5f;
  const float  fy     = py - 0.5f;
  const float  x0     = floor(fx);
  const float  y0     = floor(fy);
  const float  ax     = fx - x0;
  const float  ay     = fy - y0;
  const int    ix     = int(x0);
  const int    iy     = int(y0);
  const float4 a      = DiffusionDecode(SceneFetch(buffer, width, height, ix, iy));
  const float4 b      = DiffusionDecode(SceneFetch(buffer, width, height, ix + 1, iy));
  const float4 c      = DiffusionDecode(SceneFetch(buffer, width, height, ix, iy + 1));
  const float4 d      = DiffusionDecode(SceneFetch(buffer, width, height, ix + 1, iy + 1));
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

static float4 DiffusionBSplineWeights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float u  = 1.0f - t;
  return float4(u * u * u / 6.0f, (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f,
                (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f, t3 / 6.0f);
}

static float BSplineWeight(float4 weights, int index) {
  return index == 0 ? weights.x : (index == 1 ? weights.y : (index == 2 ? weights.z : weights.w));
}

// Cubic B-spline sample of coarse_image (the scatter image); smooth magnification.
static float4 CoarseBSpline(int width, int height, float px, float py) {
  const float  fx  = px - 0.5f;
  const float  fy  = py - 0.5f;
  const float  x0  = floor(fx);
  const float  y0  = floor(fy);
  const float4 wx  = DiffusionBSplineWeights(fx - x0);
  const float4 wy  = DiffusionBSplineWeights(fy - y0);
  const int    ix  = int(x0) - 1;
  const int    iy  = int(y0) - 1;
  float4       sum = float4(0.0f);
  for (int j = 0; j < 4; ++j) {
    float4 row = float4(0.0f);
    for (int i = 0; i < 4; ++i) {
      row += CoarseFetch(width, height, ix + i, iy + j) * BSplineWeight(wx, i);
    }
    sum += row * BSplineWeight(wy, j);
  }
  return sum;
}

// Linear AP1 with the highlight boost, a smoothstep of the peak channel in log2 exposure from low
// to high stops. Negative (out-of-gamut) light does not scatter.
static float3 DiffusionBoostHighlights(float4 linear, float gain, float low, float high) {
  const float r    = max(linear.x, 0.0f);
  const float g    = max(linear.y, 0.0f);
  const float b    = max(linear.z, 0.0f);
  const float peak = max(r, max(g, b));
  const float t    = min(max((AlcedoLog2(max(peak, 1.0e-6f)) - low) / (high - low), 0.0f), 1.0f);
  const float lift = 1.0f + gain * t * t * (3.0f - 2.0f * t);
  return float3(r * lift, g * lift, b * lift);
}

// DiffusionFilter with strength 0: the ACEScc AP1 scene to linear AP1 for the DRT.
[shader("compute")]
[numthreads(16, 16, 1)]
void diffusion_filter_decode_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                                           uniform DecodePushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const uint   index  = id.y * push.width + id.x;
  const float4 source = push.source != 0u ? Float4At(push.source)[index] : scene_image[id.xy];
  const float4 linear = DiffusionDecode(source);
  if (push.destination != 0u) {
    Float4At(push.destination)[index] = linear;
  } else {
    output_image[id.xy] = linear;
  }
}

// Base level: the average of samples x samples decoded, boosted render samples inside the
// footprint of one base texel. base_to_render maps base texel coordinates to the render.
[shader("compute")]
[numthreads(16, 16, 1)]
void diffusion_filter_reduce_boost(uint3 id: SV_DispatchThreadID,
                                   uniform ReducePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.dst_width || y >= push.dst_height) {
    return;
  }
  const float step = 1.0f / float(push.samples);
  float3      sum  = float3(0.0f);
  for (int j = 0; j < push.samples; ++j) {
    const float by = float(y) + (float(j) + 0.5f) * step;
    for (int i = 0; i < push.samples; ++i) {
      const float  bx     = float(x) + (float(i) + 0.5f) * step;
      const float2 render = DiffusionTransform(push.base_to_render[0], push.base_to_render[1],
                                               push.base_to_render[2], push.base_to_render[3],
                                               push.base_to_render[4], push.base_to_render[5],
                                               bx, by);
      const float4 linear = SceneBilinearDecoded(push.source, push.src_width, push.src_height,
                                                 render.x, render.y);
      sum += DiffusionBoostHighlights(linear, push.gain, push.low, push.high);
    }
  }
  const float inv          = step * step;
  output_image[id.xy]      = float4(sum.x * inv, sum.y * inv, sum.z * inv, 1.0f);
}

// The 13-tap downsample (Jimenez, SIGGRAPH 2014) of coarse_image (the finer level) into
// output_image. Destination texel `x` spans source [2x, 2x + 2).
[shader("compute")]
[numthreads(16, 16, 1)]
void diffusion_filter_downsample(uint3 id: SV_DispatchThreadID,
                                 uniform DownsamplePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.dst_width || y >= push.dst_height) {
    return;
  }
  const int    w       = push.src_width;
  const int    h       = push.src_height;
  const float  cx      = 2.0f * (float(x) + 0.5f);
  const float  cy      = 2.0f * (float(y) + 0.5f);
  const float4 center  = CoarseBilinear(w, h, cx, cy);
  const float4 corners = CoarseBilinear(w, h, cx - 2.0f, cy - 2.0f) +
                         CoarseBilinear(w, h, cx + 2.0f, cy - 2.0f) +
                         CoarseBilinear(w, h, cx - 2.0f, cy + 2.0f) +
                         CoarseBilinear(w, h, cx + 2.0f, cy + 2.0f);
  const float4 edges   = CoarseBilinear(w, h, cx, cy - 2.0f) + CoarseBilinear(w, h, cx - 2.0f, cy) +
                         CoarseBilinear(w, h, cx + 2.0f, cy) + CoarseBilinear(w, h, cx, cy + 2.0f);
  const float4 inner   = CoarseBilinear(w, h, cx - 1.0f, cy - 1.0f) +
                         CoarseBilinear(w, h, cx + 1.0f, cy - 1.0f) +
                         CoarseBilinear(w, h, cx - 1.0f, cy + 1.0f) +
                         CoarseBilinear(w, h, cx + 1.0f, cy + 1.0f);
  output_image[id.xy] = center * 0.125f + corners * 0.03125f + edges * 0.0625f + inner * 0.125f;
}

// output = level_weight * level + coarse_weight * tent_upsample(coarse) at the level extent.
[shader("compute")]
[numthreads(16, 16, 1)]
void diffusion_filter_upsample_accumulate(uint3 id: SV_DispatchThreadID,
                                          uniform UpsamplePushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const int    w       = push.coarse_width;
  const int    h       = push.coarse_height;
  const float  cx      = (float(x) + 0.5f) * 0.5f;
  const float  cy      = (float(y) + 0.5f) * 0.5f;
  const float4 center  = CoarseBilinear(w, h, cx, cy);
  const float4 edges   = CoarseBilinear(w, h, cx - 1.0f, cy) + CoarseBilinear(w, h, cx + 1.0f, cy) +
                         CoarseBilinear(w, h, cx, cy - 1.0f) + CoarseBilinear(w, h, cx, cy + 1.0f);
  const float4 corners = CoarseBilinear(w, h, cx - 1.0f, cy - 1.0f) +
                         CoarseBilinear(w, h, cx + 1.0f, cy - 1.0f) +
                         CoarseBilinear(w, h, cx - 1.0f, cy + 1.0f) +
                         CoarseBilinear(w, h, cx + 1.0f, cy + 1.0f);
  const float4 tent    = (center * 4.0f + edges * 2.0f + corners) * (1.0f / 16.0f);
  const float4 own     = level_image[id.xy];
  output_image[id.xy]  = own * push.level_weight + tent * push.coarse_weight;
}

// out = T * ((1 - s) * I + s * B); B is the scatter image (coarse_image) at the reference
// position of the render pixel, so every render of the same frame reads the same glow.
[shader("compute")]
[numthreads(16, 16, 1)]
void diffusion_filter_mix_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                                        uniform MixPushConstants push) {
  const int x = int(id.x);
  const int y = int(id.y);
  if (x >= push.width || y >= push.height) {
    return;
  }
  const uint   index  = uint(y) * uint(push.width) + uint(x);
  const float4 source = push.source != 0u ? Float4At(push.source)[index] : scene_image[id.xy];
  const float4 linear = DiffusionDecode(source);
  const float2 base   = DiffusionTransform(push.render_to_base[0], push.render_to_base[1],
                                           push.render_to_base[2], push.render_to_base[3],
                                           push.render_to_base[4], push.render_to_base[5],
                                           float(x) + 0.5f, float(y) + 0.5f);
  const float4 glow   = CoarseBSpline(push.scatter_width, push.scatter_height, base.x, base.y);
  const float  direct = 1.0f - push.scatter_fraction;
  const float  t      = push.transmission;
  const float  s      = push.scatter_fraction;
  const float4 result = float4(t * (direct * linear.x + s * glow.x),
                               t * (direct * linear.y + s * glow.y),
                               t * (direct * linear.z + s * glow.z), linear.w);
  if (push.destination != 0u) {
    Float4At(push.destination)[index] = result;
  } else {
    output_image[id.xy] = result;
  }
}
