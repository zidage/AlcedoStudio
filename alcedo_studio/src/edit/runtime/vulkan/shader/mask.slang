//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group mask (plan #1036, phase VK11): the kernels of opencl/shader/mask.cl. They
// evaluate a Mask of a Color Grade on the render grid (MaskEvaluate), fold the Masks of a Grade
// (MaskUnion), and write the coverage displays. The names, the bindings, and the push constant
// layouts are in include/vulkan/vulkan_mask_kernels.hpp.
//
// A Mask with a Luminance Range or a Hue Range reads the Grade input: a cached RGBA32F storage
// image, or a scene-work member (an RGBA32F row-major buffer) when its address is not 0. A zero
// `*_restricted` flag of MaskRangeParams is the full range; with both flags zero the kernel does
// not read the Grade input (the kernels without ranges of mask.cl).
//
// R8 images are UNORM: a read gives u / 255 and a write of k / 255 stores k.

#include "color/color_encoding_math.h"
#include "oklab.slangh"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> scene_image;
[[vk::binding(1, 0)]] [vk::image_format("r8")] RWTexture2D<float> source_r8;
[[vk::binding(2, 0)]] [vk::image_format("r8")] RWTexture2D<float> output_r8;
[[vk::binding(3, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> output_rgba;

// MaskRangeParams (mask_model.hpp): 36 bytes.
struct MaskRangeParams {
  uint  luminance_restricted;
  float luminance_low;
  float luminance_high;
  float luminance_softness;
  uint  hue_restricted;
  float hue_start;
  float hue_end;
  float hue_softness;
  float hue_min_chroma;
};

// AnalyticPushConstants: 128 bytes. `shape` holds center_x, center_y, major_radius,
// minor_radius, rotation, inner_feather, outer_feather of a Radial Mask (kind 0), or origin_x,
// origin_y, normal_x, normal_y, transition_distance, start_value, end_value of a Linear Gradient
// (kind 1).
struct AnalyticPushConstants {
  uint64_t        scene;                   // float4 row-major, or 0: scene_image
  float           render_to_reference[6];  // the first two rows of the 3 x 3 matrix
  uint            width;
  uint            height;
  uint            reference_width;
  uint            reference_height;
  uint            kind;
  uint            invert;
  float           shape[7];
  float           opacity;
  MaskRangeParams range;
  uint            pad;
};

// RasterPushConstants: 96 bytes. The matrix maps render pixels to texture UV (SM6 R7).
struct RasterPushConstants {
  uint64_t        scene;                    // float4 row-major, or 0: scene_image
  float           render_to_texture_uv[6];  // the first two rows of the 3 x 3 matrix
  uint            width;
  uint            height;
  uint            texture_width;
  uint            texture_height;
  uint            samples;
  uint            invert;
  float           opacity;
  MaskRangeParams range;
};

// ExtentPushConstants: 8 bytes (zero fill, Union, value display).
struct ExtentPushConstants {
  uint width;
  uint height;
};

static float4* Float4At(uint64_t address) { return (float4*)address; }

static float   FinishCoverage(float value, uint invert, float range_weight, float opacity) {
  if (invert != 0u) {
    value = 1.0f - value;
  }
  return min(max(value * range_weight * opacity, 0.0f), 1.0f);
}

static float QuantizeR8(float value) {
  return float(uint(min(max(value * 255.0f + 0.5f, 0.0f), 255.0f))) / 255.0f;
}

static float SmoothStep(float edge0, float edge1, float x) {
  const float t = min(max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static bool HasRange(MaskRangeParams range) {
  return range.luminance_restricted != 0u || range.hue_restricted != 0u;
}

// Product of the Hue Range and Luminance Range weights of the Grade input pixel @p acescc
// (MaskRangeWeight of mask.cl).
static float MaskRangeWeight(MaskRangeParams range, float4 acescc) {
  const float  kDegreesPerRadian = 57.295779513082321f;
  const float3 lab               = LinearAp1ToOkLab(AcesccToLinearAp1(acescc.xyz));
  float        weight            = 1.0f;
  if (range.hue_restricted != 0u) {
    const float chroma = sqrt(lab.y * lab.y + lab.z * lab.z);
    float       hue    = AlcedoAtan2(lab.z, lab.y) * kDegreesPerRadian;
    if (hue < 0.0f) {
      hue += 360.0f;
    }
    const float width  = range.hue_start <= range.hue_end ? range.hue_end - range.hue_start
                                                          : 360.0f - range.hue_start + range.hue_end;
    float       offset = hue - range.hue_start;
    if (offset < 0.0f) {
      offset += 360.0f;
    }
    const float distance    = offset <= width ? 0.0f : min(offset - width, 360.0f - offset);
    const float gate_offset = range.hue_min_chroma - 0.03f;
    weight *= (1.0f - SmoothStep(0.0f, range.hue_softness, distance)) *
              SmoothStep(0.015f + gate_offset, 0.045f + gate_offset, chroma);
  }
  if (range.luminance_restricted != 0u) {
    const float lightness = min(max(lab.x, 0.0f), 1.0f);
    weight *= SmoothStep(range.luminance_low - range.luminance_softness, range.luminance_low,
                         lightness) *
              (1.0f - SmoothStep(range.luminance_high,
                                 range.luminance_high + range.luminance_softness, lightness));
  }
  return weight;
}

// The range weight of render pixel @p gid: 1 for the full range, without a read of the scene.
static float RangeWeightAt(MaskRangeParams range, uint64_t scene, uint2 gid, uint width) {
  if (!HasRange(range)) {
    return 1.0f;
  }
  const float4 pixel = scene != 0u ? Float4At(scene)[gid.y * width + gid.x] : scene_image[gid];
  return MaskRangeWeight(range, pixel);
}

static float AnalyticSourceCoverage(AnalyticPushConstants push, uint2 gid) {
  const float px  = float(gid.x) + 0.5f;
  const float py  = float(gid.y) + 0.5f;
  const float rx0 = push.render_to_reference[0] * px + push.render_to_reference[1] * py +
                    push.render_to_reference[2];
  const float ry0 = push.render_to_reference[3] * px + push.render_to_reference[4] * py +
                    push.render_to_reference[5];
  const float nx  = rx0 / float(push.reference_width);
  const float ny  = ry0 / float(push.reference_height);
  if (push.kind == 0u) {
    const float c      = AlcedoCos(push.shape[4]);
    const float s      = AlcedoSin(push.shape[4]);
    const float dx     = nx - push.shape[0];
    const float dy     = ny - push.shape[1];
    const float rx     = (c * dx + s * dy) / max(push.shape[2], 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / max(push.shape[3], 1.0e-6f);
    const float radius = sqrt(rx * rx + ry * ry);
    const float inner  = max(0.0f, 1.0f - push.shape[5]);
    const float outer  = 1.0f + push.shape[6];
    return 1.0f - min(max((radius - inner) / max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
  }
  const float normal_length = sqrt(push.shape[2] * push.shape[2] + push.shape[3] * push.shape[3]);
  const float normal_x      = push.shape[2] / max(normal_length, 1.0e-6f);
  const float normal_y      = push.shape[3] / max(normal_length, 1.0e-6f);
  const float distance      = (nx - push.shape[0]) * normal_x + (ny - push.shape[1]) * normal_y;
  const float t = min(max(distance / max(push.shape[4], 1.0e-6f) + 0.5f, 0.0f), 1.0f);
  return push.shape[5] + (push.shape[6] - push.shape[5]) * t;
}

// The texel (x, y) of the coverage texture, with the coordinates clamped to the edge texels.
static float CoverageTexel(int x, int y, int width, int height) {
  return source_r8[uint2(uint(clamp(x, 0, width - 1)), uint(clamp(y, 0, height - 1)))];
}

// Bilinear read at texel centers with clamp-to-edge indices; 0 outside UV [0, 1].
static float SampleRasterMask(RasterPushConstants push, float x, float y) {
  const float u = push.render_to_texture_uv[0] * x + push.render_to_texture_uv[1] * y +
                  push.render_to_texture_uv[2];
  const float v = push.render_to_texture_uv[3] * x + push.render_to_texture_uv[4] * y +
                  push.render_to_texture_uv[5];
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
    return 0.0f;
  }
  const int   width  = int(push.texture_width);
  const int   height = int(push.texture_height);
  const float tx     = u * float(push.texture_width) - 0.5f;
  const float ty     = v * float(push.texture_height) - 0.5f;
  const float x0f    = floor(tx);
  const float y0f    = floor(ty);
  const float fx     = tx - x0f;
  const float fy     = ty - y0f;
  const int   x0     = int(x0f);
  const int   y0     = int(y0f);
  const float t00    = CoverageTexel(x0, y0, width, height);
  const float t10    = CoverageTexel(x0 + 1, y0, width, height);
  const float t01    = CoverageTexel(x0, y0 + 1, width, height);
  const float t11    = CoverageTexel(x0 + 1, y0 + 1, width, height);
  const float top    = t00 + (t10 - t00) * fx;
  const float bottom = t01 + (t11 - t01) * fx;
  return top + (bottom - top) * fy;
}

// Source coverage of render pixel @p gid: the mean of samples x samples bilinear reads.
static float RasterMaskMean(RasterPushConstants push, uint2 gid) {
  const float samples = float(push.samples);
  float       sum     = 0.0f;
  for (uint j = 0; j < push.samples; ++j) {
    for (uint i = 0; i < push.samples; ++i) {
      sum += SampleRasterMask(push, float(gid.x) + (float(i) + 0.5f) / samples,
                              float(gid.y) + (float(j) + 0.5f) / samples);
    }
  }
  return sum / (samples * samples);
}

static float RasterCoverage(RasterPushConstants push, uint2 gid) {
  const float weight = RangeWeightAt(push.range, push.scene, gid, push.width);
  return QuantizeR8(FinishCoverage(RasterMaskMean(push, gid), push.invert, weight, push.opacity));
}

// Analytic MaskEvaluate (Radial, Linear Gradient): mask_analytic_r8 and mask_analytic_ranged_r8.
[shader("compute")]
[numthreads(16, 16, 1)]
void mask_analytic_r8(uint3 id: SV_DispatchThreadID, uniform AnalyticPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  const float value  = AnalyticSourceCoverage(push, gid);
  const float weight = RangeWeightAt(push.range, push.scene, gid, push.width);
  output_r8[gid]     = QuantizeR8(FinishCoverage(value, push.invert, weight, push.opacity));
}

// Raster MaskEvaluate (AI Mask): mask_raster_r8 and mask_raster_ranged_r8.
[shader("compute")]
[numthreads(16, 16, 1)]
void mask_raster_r8(uint3 id: SV_DispatchThreadID, uniform RasterPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  output_r8[gid] = RasterCoverage(push, gid);
}

// The raster MaskEvaluate into an RGBA32F coverage display: (value, 0, 0, 1), the value of
// mask_raster_r8 (SM8.5; the range probe of a candidate, #604).
[shader("compute")]
[numthreads(16, 16, 1)]
void mask_raster_rgba32f(uint3 id: SV_DispatchThreadID, uniform RasterPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  output_rgba[gid] = float4(RasterCoverage(push, gid), 0.0f, 0.0f, 1.0f);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void mask_fill_zero_r8(uint3 id: SV_DispatchThreadID, uniform ExtentPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  output_r8[gid] = 0.0f;
}

// The maximum of the Union in place: output = max(output, source).
[shader("compute")]
[numthreads(16, 16, 1)]
void mask_union_max_r8(uint3 id: SV_DispatchThreadID, uniform ExtentPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  output_r8[gid] = QuantizeR8(max(output_r8[gid], source_r8[gid]));
}

// Probed Mask display (Mask ranges D8): the R8 value of each pixel in red, on the same grid.
[shader("compute")]
[numthreads(16, 16, 1)]
void mask_value_display_rgba32f(uint3 id: SV_DispatchThreadID, uniform ExtentPushConstants push) {
  const uint2 gid = id.xy;
  if (gid.x >= push.width || gid.y >= push.height) {
    return;
  }
  output_rgba[gid] = float4(QuantizeR8(source_r8[gid]), 0.0f, 0.0f, 1.0f);
}
