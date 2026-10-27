//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;
#include "oklab_math.h"

struct MaskAnalyticParams {
  float render_to_reference[9];
  uint  width;
  uint  height;
  uint  reference_width;
  uint  reference_height;
  uint  kind;
  float center_x;
  float center_y;
  float major_radius;
  float minor_radius;
  float rotation;
  float inner_feather;
  float outer_feather;
  uint  radial_invert;
  float origin_x;
  float origin_y;
  float normal_x;
  float normal_y;
  float transition_distance;
  float start_value;
  float end_value;
  float opacity;
};

static inline float2 Transform(constant float m[9], float x, float y) {
  return float2(m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]);
}

static inline float FinishEffectiveCoverage(float value, uint invert, float opacity) {
  if (invert != 0u) {
    value = 1.0f - value;
  }
  return min(max(value * opacity, 0.0f), 1.0f);
}

// Invert, then the range weight, then opacity, then clamp (MaskRangeParams).
static inline float FinishRangedCoverage(float value, uint invert, float range_weight,
                                         float opacity) {
  if (invert != 0u) {
    value = 1.0f - value;
  }
  return min(max(value * range_weight * opacity, 0.0f), 1.0f);
}

static inline float QuantizeR8(float value) {
  return float(uint(min(max(value * 255.0f + 0.5f, 0.0f), 255.0f))) / 255.0f;
}

/// Layout of alcedo::MaskRangeParams (mask_model.hpp), which documents the equations.
struct MaskRangeParams {
  uint  luminance_restricted;
  float luminance_low;
  float luminance_high;
  float luminance_softness;
  uint  hue_restricted;
  float hue_start;
  float hue_end;
  float hue_softness;
};

static inline float RangeSmoothStep(float edge0, float edge1, float x) {
  const float t = min(max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/// Product of the Hue Range and Luminance Range weights of the Grade input pixel @p acescc.
/// Same math as MaskRangeWeight in cuda_mask_pass.cu.
static inline float MaskRangeWeight(constant MaskRangeParams& range, float4 acescc) {
  constexpr float kDegreesPerRadian = 57.295779513082321f;
  const float3    lab               = LinearAp1ToOkLab(AcesccToLinearAp1(acescc.xyz));
  float           weight            = 1.0f;
  if (range.hue_restricted != 0u) {
    const float chroma = precise::sqrt(lab.y * lab.y + lab.z * lab.z);
    float       hue    = precise::atan2(lab.z, lab.y) * kDegreesPerRadian;
    if (hue < 0.0f) {
      hue += 360.0f;
    }
    const float width  = range.hue_start <= range.hue_end
                             ? range.hue_end - range.hue_start
                             : 360.0f - range.hue_start + range.hue_end;
    float       offset = hue - range.hue_start;
    if (offset < 0.0f) {
      offset += 360.0f;
    }
    const float distance = offset <= width ? 0.0f : min(offset - width, 360.0f - offset);
    weight *= (1.0f - RangeSmoothStep(0.0f, range.hue_softness, distance)) *
              RangeSmoothStep(0.015f, 0.045f, chroma);
  }
  if (range.luminance_restricted != 0u) {
    const float lightness = min(max(lab.x, 0.0f), 1.0f);
    weight *= RangeSmoothStep(range.luminance_low - range.luminance_softness, range.luminance_low,
                              lightness) *
              (1.0f - RangeSmoothStep(range.luminance_high,
                                      range.luminance_high + range.luminance_softness, lightness));
  }
  return weight;
}

/// Source coverage of an analytic Mask at render pixel @p gid; @p invert receives the invert flag.
static inline float AnalyticSourceCoverage(constant MaskAnalyticParams& params, uint2 gid,
                                           thread uint& invert) {
  const float2 reference =
      Transform(params.render_to_reference, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const float nx    = reference.x / static_cast<float>(params.reference_width);
  const float ny    = reference.y / static_cast<float>(params.reference_height);
  float       value = 0.0f;
  if (params.kind == 0u) {
    const float c      = cos(params.rotation);
    const float s      = sin(params.rotation);
    const float dx     = nx - params.center_x;
    const float dy     = ny - params.center_y;
    const float rx     = (c * dx + s * dy) / max(params.major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / max(params.minor_radius, 1.0e-6f);
    const float radius = sqrt(rx * rx + ry * ry);
    const float inner  = max(0.0f, 1.0f - params.inner_feather);
    const float outer  = 1.0f + params.outer_feather;
    value  = 1.0f - min(max((radius - inner) / max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
  } else {
    const float normal_length = sqrt(params.normal_x * params.normal_x + params.normal_y * params.normal_y);
    const float normal_x      = params.normal_x / max(normal_length, 1.0e-6f);
    const float normal_y      = params.normal_y / max(normal_length, 1.0e-6f);
    const float distance =
        (nx - params.origin_x) * normal_x + (ny - params.origin_y) * normal_y;
    const float t =
        min(max(distance / max(params.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value  = params.start_value + (params.end_value - params.start_value) * t;
  }
  invert = params.radial_invert;
  return value;
}

kernel void mask_analytic(texture2d<float, access::write> output [[texture(0)]],
                          constant MaskAnalyticParams& params [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  uint        invert = 0;
  const float value  = AnalyticSourceCoverage(params, gid, invert);
  output.write(float4(QuantizeR8(FinishEffectiveCoverage(value, invert, params.opacity)), 0.0f,
                      0.0f, 1.0f),
               gid);
}

/// Analytic MaskEvaluate with a restricted range: the source coverage times the range weights of
/// the Grade input pixel (@p scene, RGBA32F on the render grid). mask_analytic is the same
/// evaluation without ranges.
kernel void mask_analytic_ranged(texture2d<float, access::write> output [[texture(0)]],
                                 texture2d<float, access::read> scene [[texture(1)]],
                                 constant MaskAnalyticParams& params [[buffer(0)]],
                                 constant MaskRangeParams& range [[buffer(1)]],
                                 uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  uint        invert = 0;
  const float value  = AnalyticSourceCoverage(params, gid, invert);
  const float weight = MaskRangeWeight(range, scene.read(gid));
  output.write(
      float4(QuantizeR8(FinishRangedCoverage(value, invert, weight, params.opacity)), 0.0f, 0.0f,
             1.0f),
      gid);
}

// Raster MaskEvaluate (SM6 R7). The matrix maps render pixels to texture UV.
struct MaskRasterParams {
  float render_to_texture_uv[9];
  uint  width;
  uint  height;
  uint  texture_width;
  uint  texture_height;
  uint  samples;
  uint  invert;
  float opacity;
};

// Bilinear read at texel centers with clamp-to-edge indices; 0 outside UV [0, 1].
static inline float SampleRasterMask(texture2d<float, access::read> texture,
                                     constant MaskRasterParams& params, float x, float y) {
  const float u = params.render_to_texture_uv[0] * x + params.render_to_texture_uv[1] * y +
                  params.render_to_texture_uv[2];
  const float v = params.render_to_texture_uv[3] * x + params.render_to_texture_uv[4] * y +
                  params.render_to_texture_uv[5];
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
    return 0.0f;
  }
  const float tx    = u * float(params.texture_width) - 0.5f;
  const float ty    = v * float(params.texture_height) - 0.5f;
  const float x0f   = floor(tx);
  const float y0f   = floor(ty);
  const float fx    = tx - x0f;
  const float fy    = ty - y0f;
  const int   max_x = int(params.texture_width) - 1;
  const int   max_y = int(params.texture_height) - 1;
  const uint  x0    = uint(clamp(int(x0f), 0, max_x));
  const uint  x1    = uint(clamp(int(x0f) + 1, 0, max_x));
  const uint  y0    = uint(clamp(int(y0f), 0, max_y));
  const uint  y1    = uint(clamp(int(y0f) + 1, 0, max_y));
  const float t00   = texture.read(uint2(x0, y0)).x;
  const float t10   = texture.read(uint2(x1, y0)).x;
  const float t01   = texture.read(uint2(x0, y1)).x;
  const float t11   = texture.read(uint2(x1, y1)).x;
  const float top    = t00 + (t10 - t00) * fx;
  const float bottom = t01 + (t11 - t01) * fx;
  return top + (bottom - top) * fy;
}

/// Source coverage of render pixel @p gid: the mean of samples x samples bilinear reads.
static inline float RasterMaskMean(texture2d<float, access::read> texture,
                                   constant MaskRasterParams& params, uint2 gid) {
  const float samples = float(params.samples);
  float       sum     = 0.0f;
  for (uint j = 0; j < params.samples; ++j) {
    for (uint i = 0; i < params.samples; ++i) {
      sum += SampleRasterMask(texture, params, float(gid.x) + (float(i) + 0.5f) / samples,
                              float(gid.y) + (float(j) + 0.5f) / samples);
    }
  }
  return sum / (samples * samples);
}

kernel void mask_raster(texture2d<float, access::read> texture [[texture(0)]],
                        texture2d<float, access::write> output [[texture(1)]],
                        constant MaskRasterParams& params [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  output.write(float4(QuantizeR8(FinishEffectiveCoverage(RasterMaskMean(texture, params, gid),
                                                          params.invert, params.opacity)),
                      0.0f, 0.0f, 1.0f),
               gid);
}

/// Raster MaskEvaluate with a restricted range, as mask_analytic_ranged.
kernel void mask_raster_ranged(texture2d<float, access::read> texture [[texture(0)]],
                               texture2d<float, access::write> output [[texture(1)]],
                               texture2d<float, access::read> scene [[texture(2)]],
                               constant MaskRasterParams& params [[buffer(0)]],
                               constant MaskRangeParams& range [[buffer(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const float weight = MaskRangeWeight(range, scene.read(gid));
  output.write(float4(QuantizeR8(FinishRangedCoverage(RasterMaskMean(texture, params, gid),
                                                       params.invert, weight, params.opacity)),
                      0.0f, 0.0f, 1.0f),
               gid);
}

kernel void mask_fill_zero(texture2d<float, access::write> output [[texture(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
  output.write(float4(0.0f, 0.0f, 0.0f, 1.0f), gid);
}

kernel void mask_union_max(texture2d<float, access::read> lhs [[texture(0)]],
                           texture2d<float, access::read> rhs [[texture(1)]],
                           texture2d<float, access::write> output [[texture(2)]],
                           uint2 gid [[thread_position_in_grid]]) {
  const float a = lhs.read(gid).x;
  const float b = rhs.read(gid).x;
  output.write(float4(QuantizeR8(max(a, b)), 0.0f, 0.0f, 1.0f), gid);
}

// SM8.4: Feather and Expand of AI Mask coverage (SM8 section 5.1), the passes of
// AiMaskEdgeDispatch. R8 textures are R8Unorm: a read gives u / 255, so each read converts back
// to the integer u, and each R8 write stores v / 255 for an integer v. Every pass clamps
// coordinates to the output (replicated border).

/// Layout of AiMaskEdgeGrid in metal_ai_mask_coverage.mm. The input is placed at
/// (source_x, source_y) with source_width x source_height texels; texels outside read 0.
struct AiMaskEdgeGrid {
  int width;
  int height;
  int source_x;
  int source_y;
  int source_width;
  int source_height;
};

/// Layout of FeatherWeightsArgument in metal_ai_mask_coverage.mm (kAiMaskFeatherMaximumRadius).
struct AiMaskFeatherWeights {
  int   radius;
  float values[201];
};

static inline uint AiMaskReadPlaced(texture2d<float, access::read> input, constant AiMaskEdgeGrid& grid,
                                    int x, int y) {
  const int source_x = clamp(x, 0, grid.width - 1) - grid.source_x;
  const int source_y = clamp(y, 0, grid.height - 1) - grid.source_y;
  if (source_x < 0 || source_y < 0 || source_x >= grid.source_width ||
      source_y >= grid.source_height) {
    return 0u;
  }
  const float value = input.read(uint2(uint(source_x), uint(source_y))).x;
  return uint(clamp(rint(value * 255.0f), 0.0f, 255.0f));
}

/// One 3 x 3 dilate (dilate != 0) or erode step with the cross (square == 0) or square kernel.
kernel void ai_mask_morphology(texture2d<float, access::read> input [[texture(0)]],
                               texture2d<float, access::write> output [[texture(1)]],
                               constant AiMaskEdgeGrid& grid [[buffer(0)]],
                               constant int2& operation [[buffer(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  const bool dilate = operation.x != 0;
  const bool square = operation.y != 0;
  uint       value  = AiMaskReadPlaced(input, grid, x, y);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if ((dx == 0 && dy == 0) || (!square && dx != 0 && dy != 0)) {
        continue;
      }
      const uint neighbor = AiMaskReadPlaced(input, grid, x + dx, y + dy);
      value               = dilate ? max(value, neighbor) : min(value, neighbor);
    }
  }
  output.write(float4(float(value) / 255.0f, 0.0f, 0.0f, 1.0f), gid);
}

/// Horizontal Feather pass: float sum of the R8 values / 255 into an R32Float texture.
kernel void ai_mask_feather_rows(texture2d<float, access::read> input [[texture(0)]],
                                 texture2d<float, access::write> rows [[texture(1)]],
                                 constant AiMaskEdgeGrid& grid [[buffer(0)]],
                                 constant AiMaskFeatherWeights& weights [[buffer(1)]],
                                 uint2 gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  // The host converts with `u * float(1 / 255.0)`; 1.0f / 255.0f is the same float.
  const float scale = 1.0f / 255.0f;
  float       sum   = weights.values[0] * (float(AiMaskReadPlaced(input, grid, x, y)) * scale);
  for (int k = 1; k <= weights.radius; ++k) {
    const float left  = float(AiMaskReadPlaced(input, grid, x - k, y)) * scale;
    const float right = float(AiMaskReadPlaced(input, grid, x + k, y)) * scale;
    sum += weights.values[k] * (left + right);
  }
  rows.write(float4(sum, 0.0f, 0.0f, 1.0f), gid);
}

/// Vertical Feather pass over the float rows; quantizes like cv::saturate_cast<uchar>.
kernel void ai_mask_feather_columns(texture2d<float, access::read> rows [[texture(0)]],
                                    texture2d<float, access::write> output [[texture(1)]],
                                    constant AiMaskEdgeGrid& grid [[buffer(0)]],
                                    constant AiMaskFeatherWeights& weights [[buffer(1)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= grid.width || y >= grid.height) {
    return;
  }
  const int last = grid.height - 1;
  float     sum  = weights.values[0] * rows.read(gid).x;
  for (int k = 1; k <= weights.radius; ++k) {
    const float above = rows.read(uint2(gid.x, uint(clamp(y - k, 0, last)))).x;
    const float below = rows.read(uint2(gid.x, uint(clamp(y + k, 0, last)))).x;
    sum += weights.values[k] * (above + below);
  }
  const float quantized = clamp(rint(sum * 255.0f), 0.0f, 255.0f);
  output.write(float4(quantized / 255.0f, 0.0f, 0.0f, 1.0f), gid);
}
