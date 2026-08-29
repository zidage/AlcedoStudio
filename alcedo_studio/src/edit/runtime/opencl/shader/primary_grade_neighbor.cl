//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#define GRADE_NEIGHBOR_MAX_TAPS   64
#define GRADE_BEHAVIOR_CLARITY    13u
#define GRADE_BEHAVIOR_SHARPEN    14u
#define GRADE_BEHAVIOR_HALATION   15u
#define GRADE_BEHAVIOR_FILM_GRAIN 16u

typedef struct {
  uint  behavior;
  uint  radius;
  uint  tap_count;
  float amount;
  float threshold;
  float weights[GRADE_NEIGHBOR_MAX_TAPS];
  uint  enabled;
  uint  seed_lo;
  uint  seed_hi;
  float sigma_x;
  float sigma_y;
  float redshift[3];
  float render_to_reference[6];
  uint  use_reference_coordinates;
  int   reference_width;
  int   reference_height;
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

static inline float GradeAcesccEncode(float value) {
  const float k_a         = 9.72f;
  const float k_b         = 17.52f;
  const float offset      = 0.0000152587890625f;
  const float transition  = 0.000030517578125f;
  const float floor_value = (-16.0f + k_a) / k_b;
  if (value < 0.0f) return floor_value + value;
  if (value < transition) return (log2(offset + value * 0.5f) + k_a) / k_b;
  return (log2(value) + k_a) / k_b;
}

static inline float GradeAcesccDecode(float value) {
  const float k_a         = 9.72f;
  const float k_b         = 17.52f;
  const float offset      = 0.0000152587890625f;
  const float floor_value = (-16.0f + k_a) / k_b;
  const float threshold   = (-15.0f + k_a) / k_b;
  if (value < floor_value) return value - floor_value;
  if (value <= threshold) return (exp2(value * k_b - k_a) - offset) * 2.0f;
  return exp2(value * k_b - k_a);
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

static inline float4 GradeHalationDecode(float4 value) {
  return (float4)(GradeAcesccDecode(value.x), GradeAcesccDecode(value.y),
                  GradeAcesccDecode(value.z), value.w);
}

static inline float4 GradeHalationHorizontal(read_only image2d_t src, int2 coord,
                                             const GradeNeighborParams* params) {
  const int    radius = GradeHalationRadius(params->sigma_x);
  const float  norm   = GradeHalationNormalization(radius, params->sigma_x);
  const float4 center = GradeHalationDecode(GradeNeighborRead(src, coord));
  float4       blur   = (float4)(center.x * norm, center.y * norm, center.z * norm, center.w);
  for (int tap = 1; tap <= radius; ++tap) {
    const float  weight = GradeHalationWeight(tap, params->sigma_x) * norm;
    const float4 left   = GradeHalationDecode(GradeNeighborRead(src, coord - (int2)(tap, 0)));
    const float4 right  = GradeHalationDecode(GradeNeighborRead(src, coord + (int2)(tap, 0)));
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

static inline int2 GradeFilmReferenceCoord(int2 coord, const GradeNeighborParams* params) {
  if (params->use_reference_coordinates == 0u) return coord;
  const float x        = (float)coord.x + 0.5f;
  const float y        = (float)coord.y + 0.5f;
  const float mapped_x = params->render_to_reference[0] * x + params->render_to_reference[1] * y +
                         params->render_to_reference[2] - 0.5f;
  const float mapped_y = params->render_to_reference[3] * x + params->render_to_reference[4] * y +
                         params->render_to_reference[5] - 0.5f;
  return (int2)(clamp((int)floor(mapped_x + 0.5f), 0, max(params->reference_width, 1) - 1),
                clamp((int)floor(mapped_y + 0.5f), 0, max(params->reference_height, 1) - 1));
}

static inline float GradeFilmChannel(float4 value, int channel) {
  return channel == 0 ? value.x : (channel == 1 ? value.y : value.z);
}

static inline float GradeFilmSample(read_only image2d_t src, int2 coord, int channel,
                                    const GradeNeighborParams* params) {
  const int2 size       = get_image_dim(src);
  coord                 = clamp(coord, (int2)(0), size - (int2)(1));
  const int2  ref       = GradeFilmReferenceCoord(coord, params);
  const ulong prng_seed = ((ulong)params->seed_hi << 32u) | (ulong)params->seed_lo;
  const ulong stream    = opencl_prng_pixel_stream_2d(ref.x, ref.y, (uint)channel);
  const float draw      = opencl_prng_uniform_float01(prng_seed, stream, 0xd1b54a32d192ed03UL);
  return draw < clamp(GradeFilmChannel(GradeNeighborRead(src, coord), channel), 0.0f, 1.0f) ? 1.0f
                                                                                            : 0.0f;
}

static inline float4 GradeFilmHorizontal(read_only image2d_t src, int2 coord,
                                         const GradeNeighborParams* params) {
  float result[3];
  for (int channel = 0; channel < 3; ++channel) {
    float acc = GradeFilmSample(src, coord, channel, params) * params->weights[0];
    for (uint tap = 1u; tap < params->tap_count; ++tap) {
      const int distance = (int)tap;
      acc += (GradeFilmSample(src, coord + (int2)(-distance, 0), channel, params) +
              GradeFilmSample(src, coord + (int2)(distance, 0), channel, params)) *
             params->weights[tap];
    }
    result[channel] = acc;
  }
  return (float4)(result[0], result[1], result[2], GradeNeighborRead(src, coord).w);
}

static inline float4 GradeFilmVertical(__local const float4* tile, int center, int tile_stride,
                                       const GradeNeighborParams* params) {
  float4 blur = tile[center] * params->weights[0];
  for (uint tap = 1u; tap < params->tap_count; ++tap) {
    const int distance = (int)tap * tile_stride;
    blur += (tile[center - distance] + tile[center + distance]) * params->weights[tap];
  }
  blur.w = tile[center].w;
  return blur;
}

static inline float GradeFilmLerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float GradeFilmEvalSigma(float density, const float density_lut[11],
                                       const float sigma_lut[11]) {
  if (density <= density_lut[0]) return sigma_lut[0];
  for (int i = 0; i < 10; ++i) {
    if (density <= density_lut[i + 1]) {
      const float t =
          (density - density_lut[i]) / fmax(density_lut[i + 1] - density_lut[i], 1.0e-6f);
      return GradeFilmLerp(sigma_lut[i], sigma_lut[i + 1], t);
    }
  }
  return sigma_lut[10];
}

static inline float GradeFilmGranularity(float signal, int channel) {
  const float red_density[11]   = {0.22f, 0.22f, 0.25f, 0.42f, 0.78f, 1.19f,
                                   1.58f, 1.94f, 2.26f, 2.45f, 2.52f};
  const float red_sigma[11]     = {0.00594f, 0.00565f, 0.00524f, 0.01085f, 0.00844f, 0.00531f,
                                   0.00486f, 0.00486f, 0.00445f, 0.00440f, 0.00474f};
  const float green_density[11] = {0.59f, 0.61f, 0.66f, 0.94f, 1.36f, 1.76f,
                                   2.18f, 2.49f, 2.61f, 2.67f, 2.69f};
  const float green_sigma[11]   = {0.00517f, 0.00524f, 0.00625f, 0.01085f, 0.00823f, 0.00617f,
                                   0.00625f, 0.00691f, 0.00602f, 0.00524f, 0.00445f};
  const float blue_density[11]  = {1.00f, 1.03f, 1.10f, 1.32f, 1.51f, 1.78f,
                                   2.05f, 2.38f, 2.68f, 2.91f, 3.00f};
  const float blue_sigma[11]    = {0.01185f, 0.01261f, 0.01485f, 0.01581f, 0.01200f, 0.01099f,
                                   0.01127f, 0.01058f, 0.00844f, 0.00641f, 0.00418f};
  const float u                 = clamp(signal, 0.0f, 1.0f);
  const float density           = channel == 0   ? GradeFilmLerp(0.22f, 2.52f, u)
                                  : channel == 1 ? GradeFilmLerp(0.59f, 2.69f, u)
                                                 : GradeFilmLerp(1.00f, 3.00f, u);
  const float sigma             = channel == 0 ? GradeFilmEvalSigma(density, red_density, red_sigma)
                                  : channel == 1 ? GradeFilmEvalSigma(density, green_density, green_sigma)
                                                 : GradeFilmEvalSigma(density, blue_density, blue_sigma);
  return clamp(sigma / 0.0075f, 0.55f, 2.15f);
}

static inline float GradeFilmDensityError(float signal, float coverage, int channel) {
  return (coverage - clamp(signal, 0.0f, 1.0f)) * GradeFilmGranularity(signal, channel);
}

static inline float4 GradeFilmApply(float4 source, float4 coverage, float amount) {
  float3      density = (float3)(GradeFilmDensityError(source.x, coverage.x, 0),
                            GradeFilmDensityError(source.y, coverage.y, 1),
                            GradeFilmDensityError(source.z, coverage.z, 2));
  const float neutral = (density.x + density.y + density.z) * (1.0f / 3.0f);
  const float highlight_signal =
      (clamp(source.x, 0.0f, 1.0f) + clamp(source.y, 0.0f, 1.0f) + clamp(source.z, 0.0f, 1.0f)) *
      (1.0f / 3.0f);
  const float highlight      = GradeNeighborSmoothstep(0.72f, 0.96f, highlight_signal);
  const float chroma         = GradeFilmLerp(0.68f, 0.18f, highlight);
  density                    = (float3)(neutral) + chroma * (density - (float3)(neutral));
  const float brighten_scale = GradeFilmLerp(1.0f, 0.12f, highlight);
  density.x                  = density.x < 0.0f ? density.x * brighten_scale : density.x;
  density.y                  = density.y < 0.0f ? density.y * brighten_scale : density.y;
  density.z                  = density.z < 0.0f ? density.z * brighten_scale : density.z;
  const float strength       = amount * 1.35f;
  return (float4)(source.x - source.x * strength * density.x,
                  source.y - source.y * strength * density.y,
                  source.z - source.z * strength * density.z, source.w);
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
  } else if (params.behavior == GRADE_BEHAVIOR_FILM_GRAIN) {
    result = GradeFilmHorizontal(src, gid, &params);
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
    const float4 linear = GradeHalationDecode(source);
    const float3 spill  = fmax(blur.xyz - linear.xyz, (float3)(0.0f));
    const float3 result =
        linear.xyz + spill * params.amount *
                         (float3)(params.redshift[0], params.redshift[1], params.redshift[2]);
    write_imagef(dst, gid,
                 (float4)(GradeAcesccEncode(result.x), GradeAcesccEncode(result.y),
                          GradeAcesccEncode(result.z), source.w));
  } else {
    write_imagef(dst, gid,
                 GradeFilmApply(source,
                                GradeFilmVertical(vertical_tile, center, tile_width, &params),
                                params.amount));
  }
}
