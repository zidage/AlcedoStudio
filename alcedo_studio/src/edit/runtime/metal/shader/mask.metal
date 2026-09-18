//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

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

static inline float QuantizeR8(float value) {
  return float(uint(min(max(value * 255.0f + 0.5f, 0.0f), 255.0f))) / 255.0f;
}

kernel void mask_analytic(texture2d<float, access::write> output [[texture(0)]],
                          constant MaskAnalyticParams& params [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const float2 reference =
      Transform(params.render_to_reference, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const float nx    = reference.x / static_cast<float>(params.reference_width);
  const float ny    = reference.y / static_cast<float>(params.reference_height);
  float       value = 0.0f;
  uint        invert = 0;
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
    invert = params.radial_invert;
  } else {
    const float normal_length = sqrt(params.normal_x * params.normal_x + params.normal_y * params.normal_y);
    const float normal_x      = params.normal_x / max(normal_length, 1.0e-6f);
    const float normal_y      = params.normal_y / max(normal_length, 1.0e-6f);
    const float distance =
        (nx - params.origin_x) * normal_x + (ny - params.origin_y) * normal_y;
    const float t =
        min(max(distance / max(params.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value  = params.start_value + (params.end_value - params.start_value) * t;
    invert = params.radial_invert;
  }
  output.write(float4(QuantizeR8(FinishEffectiveCoverage(value, invert, params.opacity)), 0.0f,
                      0.0f, 1.0f),
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
