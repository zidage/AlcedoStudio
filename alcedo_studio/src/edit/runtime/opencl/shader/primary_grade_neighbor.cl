//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#define GRADE_NEIGHBOR_MAX_TAPS   64
#define GRADE_BEHAVIOR_CLARITY    13u
#define GRADE_BEHAVIOR_SHARPEN    14u
#define GRADE_BEHAVIOR_HALATION   15u

typedef struct {
  uint  behavior;
  uint  radius;
  uint  tap_count;
  float amount;
  float threshold;
  float weights[GRADE_NEIGHBOR_MAX_TAPS];
  uint  enabled;
  float sigma_x;
  float sigma_y;
  float redshift[3];
} GradeNeighborParams;

static inline float4 GradeNeighborRead(read_only image2d_t src, int2 coord) {
  return read_imagef(src, kNearestClamp, coord);
}

static inline float GradeNeighborLuma(float4 value) {
  return value.x * 0.114f + value.y * 0.587f + value.z * 0.299f;
}

static inline float GradeNeighborSmoothstep(float edge0, float edge1, float value) {
  const float t = clamp((value - edge0) / fmax(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline float4 GradeGaussianHorizontal(read_only image2d_t src, int2 coord,
                                             const GradeNeighborParams* params) {
  float4 blur = GradeNeighborRead(src, coord) * params->weights[0];
  for (uint tap = 1u; tap < params->tap_count; ++tap) {
    const int distance = (int)tap;
    blur += (GradeNeighborRead(src, coord + (int2)(distance, 0)) +
             GradeNeighborRead(src, coord - (int2)(distance, 0))) *
            params->weights[tap];
  }
  return blur;
}

static inline float4 GradeGaussianVertical(__local const float4* tile, int center, int tile_stride,
                                           const GradeNeighborParams* params) {
  float4 blur = tile[center] * params->weights[0];
  for (uint tap = 1u; tap < params->tap_count; ++tap) {
    const int distance = (int)tap * tile_stride;
    blur += (tile[center + distance] + tile[center - distance]) * params->weights[tap];
  }
  return blur;
}

static inline int GradeHalationRadius(float sigma) {
  if (!(sigma > 0.0f)) return 0;
  return clamp((int)ceil(sigma * 3.0f), 1, GRADE_NEIGHBOR_MAX_TAPS - 1);
}

static inline float GradeHalationWeight(int tap, float sigma) {
  return tap == 0 ? 1.0f : exp(-(float)tap / fmax(sigma, 1.0e-6f));
}

static inline float GradeHalationNormalization(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) sum += 2.0f * GradeHalationWeight(tap, sigma);
  return 1.0f / fmax(sum, 1.0e-6f);
}

static inline float4 GradeHalationHorizontal(read_only image2d_t src, int2 coord,
                                             const GradeNeighborParams* params) {
  const int    radius = GradeHalationRadius(params->sigma_x);
  const float  norm   = GradeHalationNormalization(radius, params->sigma_x);
  const float4 center = GradeNeighborRead(src, coord);
  float4       blur   = (float4)(center.x * norm, center.y * norm, center.z * norm, center.w);
  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight = GradeHalationWeight(tap, params->sigma_x) * norm;
    const float4 left   = GradeNeighborRead(src, coord - (int2)(tap, 0));
    const float4 right  = GradeNeighborRead(src, coord + (int2)(tap, 0));
    blur.xyz += (left.xyz + right.xyz) * weight;
  }
  return blur;
}

static inline float4 GradeHalationVertical(__local const float4* tile, int center, int tile_stride,
                                           const GradeNeighborParams* params) {
  const int   radius = GradeHalationRadius(params->sigma_y);
  const float norm   = GradeHalationNormalization(radius, params->sigma_y);
  float4      blur   = tile[center];
  blur.xyz *= norm;
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = GradeHalationWeight(tap, params->sigma_y) * norm;
    blur.xyz +=
        (tile[center - tap * tile_stride].xyz + tile[center + tap * tile_stride].xyz) * weight;
  }
  return blur;
}

static inline int GradeVerticalRadius(const GradeNeighborParams* params) {
  if (params->behavior == GRADE_BEHAVIOR_HALATION) {
    return GradeHalationRadius(params->sigma_y);
  }
  return (int)params->radius;
}

__kernel void primary_grade_neighbor_blur_h_rgba32f(read_only image2d_t  src,
                                                    write_only image2d_t dst,
                                                    GradeNeighborParams  params) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) return;
  float4 result;
  if (params.behavior == GRADE_BEHAVIOR_HALATION) {
    result = GradeHalationHorizontal(src, gid, &params);
  } else {
    result = GradeGaussianHorizontal(src, gid, &params);
  }
  write_imagef(dst, gid, result);
}

__kernel void primary_grade_neighbor_apply_v_rgba32f(read_only image2d_t  original,
                                                     read_only image2d_t  blur_horizontal,
                                                     write_only image2d_t dst,
                                                     GradeNeighborParams  params,
                                                     __local float4*      vertical_tile) {
  const int2 gid         = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size        = get_image_dim(dst);
  const int  radius      = GradeVerticalRadius(&params);
  const int  tile_width  = (int)get_local_size(0);
  const int  tile_height = (int)get_local_size(1) + 2 * radius;
  const int  local_index = (int)get_local_id(1) * tile_width + (int)get_local_id(0);
  const int  local_count = tile_width * (int)get_local_size(1);
  for (int tile_index = local_index; tile_index < tile_width * tile_height;
       tile_index += local_count) {
    const int tile_x   = tile_index % tile_width;
    const int tile_y   = tile_index / tile_width;
    const int source_x = (int)get_group_id(0) * tile_width + tile_x;
    const int source_y =
        clamp((int)get_group_id(1) * (int)get_local_size(1) + tile_y - radius, 0, size.y - 1);
    vertical_tile[tile_index] = source_x < size.x
                                    ? GradeNeighborRead(blur_horizontal, (int2)(source_x, source_y))
                                    : (float4)(0.0f);
  }
  barrier(CLK_LOCAL_MEM_FENCE);
  if (gid.x >= size.x || gid.y >= size.y) return;
  const float4 source = GradeNeighborRead(original, gid);
  const int    center = ((int)get_local_id(1) + radius) * tile_width + (int)get_local_id(0);
  if (params.behavior == GRADE_BEHAVIOR_SHARPEN) {
    const float4 blur = GradeGaussianVertical(vertical_tile, center, tile_width, &params);
    float4       high = (float4)(source.xyz - blur.xyz, 0.0f);
    if (params.threshold > 0.0f && fabs(GradeNeighborLuma(high)) <= params.threshold) {
      high = (float4)(0.0f);
    }
    write_imagef(dst, gid, (float4)(source.xyz + high.xyz * params.amount, source.w));
  } else if (params.behavior == GRADE_BEHAVIOR_CLARITY) {
    const float4 blur = GradeGaussianVertical(vertical_tile, center, tile_width, &params);
    const float4 diff = (float4)(source.xyz - blur.xyz, 0.0f);
    const float  protect =
        1.0f - GradeNeighborSmoothstep(0.0f, 0.18f, fabs(GradeNeighborLuma(diff)));
    const float centered = (GradeNeighborLuma(source) - 0.5f) * 2.0f;
    const float strength = params.amount * protect * fmax(1.0f - centered * centered, 0.0f);
    write_imagef(dst, gid, (float4)(fma(diff.xyz, (float3)(strength), source.xyz), source.w));
  } else if (params.behavior == GRADE_BEHAVIOR_HALATION) {
    const float4 blur   = GradeHalationVertical(vertical_tile, center, tile_width, &params);
    const float3 spill  = fmax(blur.xyz - source.xyz, (float3)(0.0f));
    const float3 result =
        source.xyz + spill * params.amount *
                         (float3)(params.redshift[0], params.redshift[1], params.redshift[2]);
    write_imagef(dst, gid, (float4)(result, source.w));
  }
}

static inline float4 SceneTap(read_only image2d_t image, __global const float4* buffer,
                              int is_buffer, int2 coord, int width, int height) {
  coord.x = clamp(coord.x, 0, width - 1);
  coord.y = clamp(coord.y, 0, height - 1);
  if (is_buffer != 0) {
    return buffer[(uint)coord.y * (uint)width + (uint)coord.x];
  }
  return GradeNeighborRead(image, coord);
}

static inline float4 SceneMix(float4 adjusted, read_only image2d_t mix_image,
                              __global const float4* mix_buffer, int mix_is_buffer,
                              read_only image2d_t mask, int has_mask, float grade_mix, int2 gid,
                              int width, int height) {
  if (mix_is_buffer < 0) {
    return adjusted;
  }
  const float4 original = SceneTap(mix_image, mix_buffer, mix_is_buffer, gid, width, height);
  float mix = grade_mix;
  if (has_mask != 0) {
    mix *= read_imagef(mask, kNearestClamp, gid).x;
  }
  mix = clamp(mix, 0.0f, 1.0f);
  return (float4)(original.xyz + (adjusted.xyz - original.xyz) * mix, original.w);
}

__kernel void primary_grade_neighbor_blur_h_scene_rgba32f(
    read_only image2d_t src_image, __global const float4* src_buffer, int src_is_buffer,
    write_only image2d_t dst, GradeNeighborParams params, uint width, uint height) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) return;
  float4 result;
  if (params.behavior == GRADE_BEHAVIOR_HALATION) {
    const int radius = GradeHalationRadius(params.sigma_x);
    const float norm = GradeHalationNormalization(radius, params.sigma_x);
    result = SceneTap(src_image, src_buffer, src_is_buffer, gid, (int)width, (int)height) * norm;
    for (int tap = 1; tap <= radius; ++tap) {
      const float weight = GradeHalationWeight(tap, params.sigma_x) * norm;
      result += (SceneTap(src_image, src_buffer, src_is_buffer, gid + (int2)(tap, 0), (int)width,
                          (int)height) +
                 SceneTap(src_image, src_buffer, src_is_buffer, gid - (int2)(tap, 0), (int)width,
                          (int)height)) *
                weight;
    }
  } else {
    result = SceneTap(src_image, src_buffer, src_is_buffer, gid, (int)width, (int)height) *
             params.weights[0];
    for (uint tap = 1u; tap < params.tap_count; ++tap) {
      const int distance = (int)tap;
      result += (SceneTap(src_image, src_buffer, src_is_buffer, gid + (int2)(distance, 0),
                          (int)width, (int)height) +
                 SceneTap(src_image, src_buffer, src_is_buffer, gid - (int2)(distance, 0),
                          (int)width, (int)height)) *
                params.weights[tap];
    }
  }
  write_imagef(dst, gid, result);
}

__kernel void primary_grade_neighbor_apply_v_scene_rgba32f(
    read_only image2d_t original_image, __global const float4* original_buffer, int original_is_buffer,
    read_only image2d_t blur_horizontal, write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, read_only image2d_t mix_image,
    __global const float4* mix_buffer, int mix_is_buffer, read_only image2d_t mask, int has_mask,
    float grade_mix, GradeNeighborParams params, uint width, uint height,
    __local float4* vertical_tile) {
  const int2 gid         = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int  radius      = GradeVerticalRadius(&params);
  const int  tile_width  = (int)get_local_size(0);
  const int  tile_height = (int)get_local_size(1) + 2 * radius;
  const int  local_index = (int)get_local_id(1) * tile_width + (int)get_local_id(0);
  const int  local_count = tile_width * (int)get_local_size(1);
  for (int tile_index = local_index; tile_index < tile_width * tile_height;
       tile_index += local_count) {
    const int tile_x   = tile_index % tile_width;
    const int tile_y   = tile_index / tile_width;
    const int source_x = (int)get_group_id(0) * tile_width + tile_x;
    const int source_y =
        clamp((int)get_group_id(1) * (int)get_local_size(1) + tile_y - radius, 0, (int)height - 1);
    vertical_tile[tile_index] =
        source_x < (int)width ? GradeNeighborRead(blur_horizontal, (int2)(source_x, source_y))
                              : (float4)(0.0f);
  }
  barrier(CLK_LOCAL_MEM_FENCE);
  if (gid.x >= (int)width || gid.y >= (int)height) return;
  const float4 source =
      SceneTap(original_image, original_buffer, original_is_buffer, gid, (int)width, (int)height);
  const int center = ((int)get_local_id(1) + radius) * tile_width + (int)get_local_id(0);
  // The host sends no other behavior (RequireOpenClNeighborhoodKernelBehavior).
  float4 adjusted = source;
  if (params.behavior == GRADE_BEHAVIOR_SHARPEN) {
    const float4 blur = GradeGaussianVertical(vertical_tile, center, tile_width, &params);
    float4       high = (float4)(source.xyz - blur.xyz, 0.0f);
    if (params.threshold > 0.0f && fabs(GradeNeighborLuma(high)) <= params.threshold) {
      high = (float4)(0.0f);
    }
    adjusted = (float4)(source.xyz + high.xyz * params.amount, source.w);
  } else if (params.behavior == GRADE_BEHAVIOR_CLARITY) {
    const float4 blur = GradeGaussianVertical(vertical_tile, center, tile_width, &params);
    const float4 diff = (float4)(source.xyz - blur.xyz, 0.0f);
    const float  protect =
        1.0f - GradeNeighborSmoothstep(0.0f, 0.18f, fabs(GradeNeighborLuma(diff)));
    const float centered = (GradeNeighborLuma(source) - 0.5f) * 2.0f;
    const float strength = params.amount * protect * fmax(1.0f - centered * centered, 0.0f);
    adjusted = (float4)(fma(diff.xyz, (float3)(strength), source.xyz), source.w);
  } else if (params.behavior == GRADE_BEHAVIOR_HALATION) {
    const float4 blur  = GradeHalationVertical(vertical_tile, center, tile_width, &params);
    const float3 spill = fmax(blur.xyz - source.xyz, (float3)(0.0f));
    const float3 result =
        source.xyz + spill * params.amount *
                         (float3)(params.redshift[0], params.redshift[1], params.redshift[2]);
    adjusted = (float4)(result, source.w);
  }
  adjusted = SceneMix(adjusted, mix_image, mix_buffer, mix_is_buffer, mask, has_mask, grade_mix, gid,
                      (int)width, (int)height);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)gid.y * width + (uint)gid.x] = adjusted;
  } else {
    write_imagef(dst_image, gid, adjusted);
  }
}
