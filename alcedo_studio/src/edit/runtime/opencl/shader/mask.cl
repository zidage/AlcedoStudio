//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

typedef struct {
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
} MaskAnalyticParams;

static inline float2 Transform(__private const float* matrix, float x, float y) {
  return (float2)(matrix[0] * x + matrix[1] * y + matrix[2],
                  matrix[3] * x + matrix[4] * y + matrix[5]);
}

static inline float FinishEffectiveCoverage(float value, uint invert, float opacity) {
  if (invert != 0u) {
    value = 1.0f - value;
  }
  return min(max(value * opacity, 0.0f), 1.0f);
}

static inline float QuantizeR8(float value) {
  return (float)((uint)min(max(value * 255.0f + 0.5f, 0.0f), 255.0f)) / 255.0f;
}

__kernel void mask_analytic_r8(__write_only image2d_t output, MaskAnalyticParams params) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const float2 reference =
      Transform(params.render_to_reference, (float)gid.x + 0.5f, (float)gid.y + 0.5f);
  const float nx     = reference.x / (float)params.reference_width;
  const float ny     = reference.y / (float)params.reference_height;
  float       value  = 0.0f;
  uint        invert = 0u;
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
    value = 1.0f - min(max((radius - inner) / max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
    invert = params.radial_invert;
  } else {
    const float normal_length = sqrt(params.normal_x * params.normal_x +
                                     params.normal_y * params.normal_y);
    const float normal_x = params.normal_x / max(normal_length, 1.0e-6f);
    const float normal_y = params.normal_y / max(normal_length, 1.0e-6f);
    const float distance = (nx - params.origin_x) * normal_x +
                           (ny - params.origin_y) * normal_y;
    const float t = min(max(distance / max(params.transition_distance, 1.0e-6f) + 0.5f,
                             0.0f),
                        1.0f);
    value  = params.start_value + (params.end_value - params.start_value) * t;
    invert = params.radial_invert;
  }
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(FinishEffectiveCoverage(value, invert, params.opacity)), 0.0f,
                        0.0f, 1.0f));
}

__kernel void mask_fill_zero_r8(__write_only image2d_t output, uint width, uint height) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= width || gid.y >= height) {
    return;
  }
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(0.0f, 0.0f, 0.0f, 1.0f));
}

__kernel void mask_union_max_r8(__read_only image2d_t lhs, __read_only image2d_t rhs,
                                __write_only image2d_t output, uint width, uint height) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= width || gid.y >= height) {
    return;
  }
  const float a = read_imagef(lhs, kNearestClamp, (int2)(gid.x, gid.y)).x;
  const float b = read_imagef(rhs, kNearestClamp, (int2)(gid.x, gid.y)).x;
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(max(a, b)), 0.0f, 0.0f, 1.0f));
}
