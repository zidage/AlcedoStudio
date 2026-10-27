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

// Invert, then the range weight, then opacity, then clamp (MaskRangeParams).
static inline float FinishRangedCoverage(float value, uint invert, float range_weight,
                                         float opacity) {
  if (invert != 0u) {
    value = 1.0f - value;
  }
  return min(max(value * range_weight * opacity, 0.0f), 1.0f);
}

static inline float QuantizeR8(float value) {
  return (float)((uint)min(max(value * 255.0f + 0.5f, 0.0f), 255.0f)) / 255.0f;
}

// Layout of alcedo::MaskRangeParams (mask_model.hpp), which documents the equations.
typedef struct {
  uint  luminance_restricted;
  float luminance_low;
  float luminance_high;
  float luminance_softness;
  uint  hue_restricted;
  float hue_start;
  float hue_end;
  float hue_softness;
} MaskRangeParams;

static inline float SmoothStep(float edge0, float edge1, float x) {
  const float t = min(max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Product of the Hue Range and Luminance Range weights of the Grade input pixel @p acescc.
// Same math as MaskRangeWeight in cuda_mask_pass.cu.
static inline float MaskRangeWeight(__private const MaskRangeParams* range, float4 acescc) {
  const float  kDegreesPerRadian = 57.295779513082321f;
  const float3 lab = LinearAp1ToOkLab(AcesccToLinearAp1(acescc.xyz));
  float        weight = 1.0f;
  if (range->hue_restricted != 0u) {
    const float chroma = sqrt(lab.y * lab.y + lab.z * lab.z);
    float       hue    = atan2(lab.z, lab.y) * kDegreesPerRadian;
    if (hue < 0.0f) {
      hue += 360.0f;
    }
    const float width  = range->hue_start <= range->hue_end
                             ? range->hue_end - range->hue_start
                             : 360.0f - range->hue_start + range->hue_end;
    float       offset = hue - range->hue_start;
    if (offset < 0.0f) {
      offset += 360.0f;
    }
    const float distance = offset <= width ? 0.0f : min(offset - width, 360.0f - offset);
    weight *= (1.0f - SmoothStep(0.0f, range->hue_softness, distance)) *
              SmoothStep(0.015f, 0.045f, chroma);
  }
  if (range->luminance_restricted != 0u) {
    const float lightness = min(max(lab.x, 0.0f), 1.0f);
    weight *= SmoothStep(range->luminance_low - range->luminance_softness, range->luminance_low,
                         lightness) *
              (1.0f - SmoothStep(range->luminance_high,
                                 range->luminance_high + range->luminance_softness, lightness));
  }
  return weight;
}

// Grade input pixel from an image or a scene-work buffer (OpenClSceneView).
static inline float4 RangeSceneRead(__read_only image2d_t image, __global const float4* buffer,
                                    int is_buffer, uint2 gid, uint width) {
  if (is_buffer != 0) {
    return buffer[gid.y * width + gid.x];
  }
  return read_imagef(image, kNearestClamp, (int2)(gid.x, gid.y));
}

static inline float AnalyticSourceCoverage(__private const MaskAnalyticParams* params, uint2 gid,
                                           uint* invert) {
  const float2 reference =
      Transform(params->render_to_reference, (float)gid.x + 0.5f, (float)gid.y + 0.5f);
  const float nx    = reference.x / (float)params->reference_width;
  const float ny    = reference.y / (float)params->reference_height;
  float       value = 0.0f;
  if (params->kind == 0u) {
    const float c      = cos(params->rotation);
    const float s      = sin(params->rotation);
    const float dx     = nx - params->center_x;
    const float dy     = ny - params->center_y;
    const float rx     = (c * dx + s * dy) / max(params->major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / max(params->minor_radius, 1.0e-6f);
    const float radius = sqrt(rx * rx + ry * ry);
    const float inner  = max(0.0f, 1.0f - params->inner_feather);
    const float outer  = 1.0f + params->outer_feather;
    value = 1.0f - min(max((radius - inner) / max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
  } else {
    const float normal_length = sqrt(params->normal_x * params->normal_x +
                                     params->normal_y * params->normal_y);
    const float normal_x = params->normal_x / max(normal_length, 1.0e-6f);
    const float normal_y = params->normal_y / max(normal_length, 1.0e-6f);
    const float distance = (nx - params->origin_x) * normal_x +
                           (ny - params->origin_y) * normal_y;
    const float t = min(max(distance / max(params->transition_distance, 1.0e-6f) + 0.5f,
                             0.0f),
                        1.0f);
    value = params->start_value + (params->end_value - params->start_value) * t;
  }
  *invert = params->radial_invert;
  return value;
}

__kernel void mask_analytic_r8(__write_only image2d_t output, MaskAnalyticParams params) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  uint        invert = 0u;
  const float value  = AnalyticSourceCoverage(&params, gid, &invert);
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(FinishEffectiveCoverage(value, invert, params.opacity)), 0.0f,
                        0.0f, 1.0f));
}

// Analytic MaskEvaluate with a restricted range: the source coverage times the range weights of
// the Grade input pixel. mask_analytic_r8 is the same evaluation without ranges.
__kernel void mask_analytic_ranged_r8(__write_only image2d_t output, MaskAnalyticParams params,
                                      __read_only image2d_t scene_image,
                                      __global const float4* scene_buffer, int scene_is_buffer,
                                      MaskRangeParams range) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  uint        invert = 0u;
  const float value  = AnalyticSourceCoverage(&params, gid, &invert);
  const float weight = MaskRangeWeight(
      &range, RangeSceneRead(scene_image, scene_buffer, scene_is_buffer, gid, params.width));
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(FinishRangedCoverage(value, invert, weight, params.opacity)),
                        0.0f, 0.0f, 1.0f));
}

// Raster MaskEvaluate (SM6 R7). The matrix maps render pixels to texture UV.
typedef struct {
  float render_to_texture_uv[9];
  uint  width;
  uint  height;
  uint  texture_width;
  uint  texture_height;
  uint  samples;
  uint  invert;
  float opacity;
} MaskRasterParams;

// Bilinear read at texel centers with clamp-to-edge indices; 0 outside UV [0, 1].
static inline float SampleRasterMask(__read_only image2d_t texture,
                                     __private const MaskRasterParams* params, float x, float y) {
  const float u = params->render_to_texture_uv[0] * x + params->render_to_texture_uv[1] * y +
                  params->render_to_texture_uv[2];
  const float v = params->render_to_texture_uv[3] * x + params->render_to_texture_uv[4] * y +
                  params->render_to_texture_uv[5];
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) {
    return 0.0f;
  }
  const float tx  = u * (float)params->texture_width - 0.5f;
  const float ty  = v * (float)params->texture_height - 0.5f;
  const float x0f = floor(tx);
  const float y0f = floor(ty);
  const float fx  = tx - x0f;
  const float fy  = ty - y0f;
  const int   x0  = (int)x0f;
  const int   y0  = (int)y0f;
  // The sampler clamps the integer coordinates to the edge texels.
  const float t00 = read_imagef(texture, kNearestClamp, (int2)(x0, y0)).x;
  const float t10 = read_imagef(texture, kNearestClamp, (int2)(x0 + 1, y0)).x;
  const float t01 = read_imagef(texture, kNearestClamp, (int2)(x0, y0 + 1)).x;
  const float t11 = read_imagef(texture, kNearestClamp, (int2)(x0 + 1, y0 + 1)).x;
  const float top    = t00 + (t10 - t00) * fx;
  const float bottom = t01 + (t11 - t01) * fx;
  return top + (bottom - top) * fy;
}

// Source coverage of render pixel @p gid: the mean of samples x samples bilinear reads.
static inline float RasterMaskMean(__read_only image2d_t texture,
                                   __private const MaskRasterParams* params, uint2 gid) {
  const float samples = (float)params->samples;
  float       sum     = 0.0f;
  for (uint j = 0; j < params->samples; ++j) {
    for (uint i = 0; i < params->samples; ++i) {
      sum += SampleRasterMask(texture, params, (float)gid.x + ((float)i + 0.5f) / samples,
                              (float)gid.y + ((float)j + 0.5f) / samples);
    }
  }
  return sum / (samples * samples);
}

__kernel void mask_raster_r8(__read_only image2d_t texture, __write_only image2d_t output,
                             MaskRasterParams params) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(FinishEffectiveCoverage(RasterMaskMean(texture, &params, gid),
                                                           params.invert, params.opacity)),
                        0.0f, 0.0f, 1.0f));
}

// Raster MaskEvaluate with a restricted range, as mask_analytic_ranged_r8.
__kernel void mask_raster_ranged_r8(__read_only image2d_t texture, __write_only image2d_t output,
                                    MaskRasterParams params, __read_only image2d_t scene_image,
                                    __global const float4* scene_buffer, int scene_is_buffer,
                                    MaskRangeParams range) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const float weight = MaskRangeWeight(
      &range, RangeSceneRead(scene_image, scene_buffer, scene_is_buffer, gid, params.width));
  write_imagef(output, (int2)(gid.x, gid.y),
               (float4)(QuantizeR8(FinishRangedCoverage(RasterMaskMean(texture, &params, gid),
                                                        params.invert, weight, params.opacity)),
                        0.0f, 0.0f, 1.0f));
}

__kernel void mask_fill_zero_r8(__write_only image2d_t output, uint width, uint height) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= width || gid.y >= height) {
    return;
  }
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(0.0f, 0.0f, 0.0f, 1.0f));
}

// Probed Mask display (Mask ranges D8): the R8 value of each pixel in red, on the same grid.
__kernel void mask_value_display_rgba32f(__read_only image2d_t mask, __write_only image2d_t output,
                                         uint width, uint height) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= width || gid.y >= height) {
    return;
  }
  const float value = read_imagef(mask, kNearestClamp, (int2)(gid.x, gid.y)).x;
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(QuantizeR8(value), 0.0f, 0.0f, 1.0f));
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
