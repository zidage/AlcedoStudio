//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

__kernel void drt_display_rgba32f(__read_only image2d_t src, __write_only image2d_t dst,
                                  __global const uchar* params_bytes,
                                  const uint params_offset_bytes) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }

  const __global OpenClToOutputParams* params =
      (__global const OpenClToOutputParams*)(params_bytes + params_offset_bytes);
  const float4 source = read_imagef(src, kNearestClamp, gid);
  // Input is linear AP1; the DiffusionFilter pass already decoded ACEScc.
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const float3 scene = (float3)(compressed.r, compressed.g, compressed.b);
  float3 display_linear;
  if (params->method_ == 0) {
    display_linear = opencl_aces_output_transform_fwd(scene, &params->aces_params_);
  } else {
    display_linear = opencl_open_drt_transform_fwd(scene, &params->open_drt_params_);
  }
  const float3 encoded = opencl_display_encoding(
      display_linear, params->limit_to_display_matx, params->eotf_, params->display_linear_scale_);
  write_imagef(dst, gid, (float4)(encoded.x, encoded.y, encoded.z, source.w));
}

__kernel void drt_display_scene_rgba32f(__read_only image2d_t src_image,
                                        __global const float4* src_buffer, int src_is_buffer,
                                        __write_only image2d_t dst_image, __global float4* dst_buffer,
                                        int dst_is_buffer, __global const uchar* params_bytes,
                                        const uint params_offset_bytes, uint width, uint height) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) {
    return;
  }
  const __global OpenClToOutputParams* params =
      (__global const OpenClToOutputParams*)(params_bytes + params_offset_bytes);
  const float4 source = src_is_buffer != 0
                            ? src_buffer[(uint)gid.y * width + (uint)gid.x]
                            : read_imagef(src_image, kNearestClamp, gid);
  // Input is linear AP1; the DiffusionFilter pass already decoded ACEScc.
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const float3 scene = (float3)(compressed.r, compressed.g, compressed.b);
  float3 display_linear;
  if (params->method_ == 0) {
    display_linear = opencl_aces_output_transform_fwd(scene, &params->aces_params_);
  } else {
    display_linear = opencl_open_drt_transform_fwd(scene, &params->open_drt_params_);
  }
  const float3 encoded = opencl_display_encoding(
      display_linear, params->limit_to_display_matx, params->eotf_, params->display_linear_scale_);
  const float4 outp = (float4)(encoded.x, encoded.y, encoded.z, source.w);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)gid.y * width + (uint)gid.x] = outp;
  } else {
    write_imagef(dst_image, gid, outp);
  }
}

/// DiffusionFilter with strength 0: decode the ACEScc AP1 scene to linear AP1 for the DRT.
__kernel void diffusion_filter_decode_scene_rgba32f(__read_only image2d_t src_image,
                                                    __global const float4* src_buffer,
                                                    int src_is_buffer,
                                                    __write_only image2d_t dst_image,
                                                    __global float4* dst_buffer, int dst_is_buffer,
                                                    uint width, uint height) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  if (gid.x >= (int)width || gid.y >= (int)height) {
    return;
  }
  const float4 source = src_is_buffer != 0
                            ? src_buffer[(uint)gid.y * width + (uint)gid.x]
                            : read_imagef(src_image, kNearestClamp, gid);
  const float4 linear = (float4)(CeAcesccDecode(source.x), CeAcesccDecode(source.y),
                                 CeAcesccDecode(source.z), source.w);
  if (dst_is_buffer != 0) {
    dst_buffer[(uint)gid.y * width + (uint)gid.x] = linear;
  } else {
    write_imagef(dst_image, gid, linear);
  }
}

// === DiffusionFilter scatter ==================================================
// Mirrors cuda_diffusion_filter_pass.cu. Texel `i` covers [i, i + 1); every read clamps to edge.

typedef struct {
  int   src_width;
  int   src_height;
  int   dst_width;
  int   dst_height;
  int   samples;
  float gain;
  float knee;
  float pad0;
  float base_to_render[12];
} DiffusionReduceParams;

typedef struct {
  int   width;
  int   height;
  float scatter_fraction;
  float transmission;
  float render_to_base[12];
} DiffusionMixParams;

static inline float2 DiffusionTransform(const float* m, float x, float y) {
  return (float2)(m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]);
}

static inline float4 DiffusionDecode(float4 value) {
  return (float4)(CeAcesccDecode(value.x), CeAcesccDecode(value.y),
                  CeAcesccDecode(value.z), value.w);
}

static inline float4 DiffusionSceneFetch(__read_only image2d_t image,
                                         __global const float4* buffer, int is_buffer, int width,
                                         int height, int x, int y) {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return is_buffer != 0 ? buffer[y * width + x] : read_imagef(image, kNearestClamp, (int2)(x, y));
}

/// Bilinear sample with clamp-to-edge addressing.
static inline float4 DiffusionBilinear(__read_only image2d_t image, float px, float py) {
  const float  fx     = px - 0.5f;
  const float  fy     = py - 0.5f;
  const float  x0     = floor(fx);
  const float  y0     = floor(fy);
  const float  ax     = fx - x0;
  const float  ay     = fy - y0;
  const int    ix     = (int)x0;
  const int    iy     = (int)y0;
  const float4 a      = read_imagef(image, kNearestClamp, (int2)(ix, iy));
  const float4 b      = read_imagef(image, kNearestClamp, (int2)(ix + 1, iy));
  const float4 c      = read_imagef(image, kNearestClamp, (int2)(ix, iy + 1));
  const float4 d      = read_imagef(image, kNearestClamp, (int2)(ix + 1, iy + 1));
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

/// Bilinear sample of the ACEScc scene, decoded to linear AP1 per tap before interpolation.
static inline float4 DiffusionBilinearDecoded(__read_only image2d_t image,
                                              __global const float4* buffer, int is_buffer,
                                              int width, int height, float px, float py) {
  const float  fx = px - 0.5f;
  const float  fy = py - 0.5f;
  const float  x0 = floor(fx);
  const float  y0 = floor(fy);
  const float  ax = fx - x0;
  const float  ay = fy - y0;
  const int    ix = (int)x0;
  const int    iy = (int)y0;
  const float4 a =
      DiffusionDecode(DiffusionSceneFetch(image, buffer, is_buffer, width, height, ix, iy));
  const float4 b =
      DiffusionDecode(DiffusionSceneFetch(image, buffer, is_buffer, width, height, ix + 1, iy));
  const float4 c =
      DiffusionDecode(DiffusionSceneFetch(image, buffer, is_buffer, width, height, ix, iy + 1));
  const float4 d = DiffusionDecode(
      DiffusionSceneFetch(image, buffer, is_buffer, width, height, ix + 1, iy + 1));
  const float4 top    = a * (1.0f - ax) + b * ax;
  const float4 bottom = c * (1.0f - ax) + d * ax;
  return top * (1.0f - ay) + bottom * ay;
}

static inline void DiffusionBSplineWeights(float t, float* weights) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float u  = 1.0f - t;
  weights[0]     = u * u * u / 6.0f;
  weights[1]     = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
  weights[2]     = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
  weights[3]     = t3 / 6.0f;
}

/// Cubic B-spline sample; smooth magnification of the coarse scatter image.
static inline float4 DiffusionSampleBSpline(__read_only image2d_t image, float px, float py) {
  const float fx = px - 0.5f;
  const float fy = py - 0.5f;
  const float x0 = floor(fx);
  const float y0 = floor(fy);
  float       wx[4];
  float       wy[4];
  DiffusionBSplineWeights(fx - x0, wx);
  DiffusionBSplineWeights(fy - y0, wy);
  const int ix  = (int)x0 - 1;
  const int iy  = (int)y0 - 1;
  float4    sum = (float4)(0.0f);
  for (int j = 0; j < 4; ++j) {
    float4 row = (float4)(0.0f);
    for (int i = 0; i < 4; ++i) {
      row += read_imagef(image, kNearestClamp, (int2)(ix + i, iy + j)) * wx[i];
    }
    sum += row * wy[j];
  }
  return sum;
}

/// Linear AP1 with the near-clip highlight boost. Negative (out-of-gamut) light does not scatter.
static inline float3 DiffusionBoostHighlights(float4 linear, float gain, float knee) {
  const float r    = fmax(linear.x, 0.0f);
  const float g    = fmax(linear.y, 0.0f);
  const float b    = fmax(linear.z, 0.0f);
  const float peak = fmax(r, fmax(g, b));
  const float t    = fmin(fmax((peak - knee) / (1.0f - knee), 0.0f), 1.0f);
  const float lift = 1.0f + gain * t * t * (3.0f - 2.0f * t);
  return (float3)(r * lift, g * lift, b * lift);
}

/// Base level: average of samples x samples decoded, boosted render samples inside the footprint
/// of one base texel. base_to_render maps base texel coordinates to the render.
__kernel void diffusion_filter_reduce_boost(__read_only image2d_t src_image,
                                            __global const float4* src_buffer, int src_is_buffer,
                                            __write_only image2d_t dst,
                                            DiffusionReduceParams params) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.dst_width || y >= params.dst_height) {
    return;
  }
  const float step = 1.0f / (float)params.samples;
  float3      sum  = (float3)(0.0f);
  for (int j = 0; j < params.samples; ++j) {
    const float by = (float)y + ((float)j + 0.5f) * step;
    for (int i = 0; i < params.samples; ++i) {
      const float  bx     = (float)x + ((float)i + 0.5f) * step;
      const float2 render = DiffusionTransform(params.base_to_render, bx, by);
      const float4 linear =
          DiffusionBilinearDecoded(src_image, src_buffer, src_is_buffer, params.src_width,
                                   params.src_height, render.x, render.y);
      sum += DiffusionBoostHighlights(linear, params.gain, params.knee);
    }
  }
  const float inv = step * step;
  write_imagef(dst, (int2)(x, y), (float4)(sum.x * inv, sum.y * inv, sum.z * inv, 1.0f));
}

/// 13-tap downsample (Jimenez, SIGGRAPH 2014). Destination texel `x` spans source [2x, 2x + 2).
__kernel void diffusion_filter_downsample(__read_only image2d_t src, __write_only image2d_t dst,
                                          int dst_width, int dst_height) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= dst_width || y >= dst_height) {
    return;
  }
  const float  cx      = 2.0f * ((float)x + 0.5f);
  const float  cy      = 2.0f * ((float)y + 0.5f);
  const float4 center  = DiffusionBilinear(src, cx, cy);
  const float4 corners = DiffusionBilinear(src, cx - 2.0f, cy - 2.0f) +
                         DiffusionBilinear(src, cx + 2.0f, cy - 2.0f) +
                         DiffusionBilinear(src, cx - 2.0f, cy + 2.0f) +
                         DiffusionBilinear(src, cx + 2.0f, cy + 2.0f);
  const float4 edges   = DiffusionBilinear(src, cx, cy - 2.0f) +
                         DiffusionBilinear(src, cx - 2.0f, cy) +
                         DiffusionBilinear(src, cx + 2.0f, cy) +
                         DiffusionBilinear(src, cx, cy + 2.0f);
  const float4 inner   = DiffusionBilinear(src, cx - 1.0f, cy - 1.0f) +
                         DiffusionBilinear(src, cx + 1.0f, cy - 1.0f) +
                         DiffusionBilinear(src, cx - 1.0f, cy + 1.0f) +
                         DiffusionBilinear(src, cx + 1.0f, cy + 1.0f);
  write_imagef(dst, (int2)(x, y),
               center * 0.125f + corners * 0.03125f + edges * 0.0625f + inner * 0.125f);
}

/// dst = level_weight * level + coarse_weight * tent_upsample(coarse) at the level extent.
__kernel void diffusion_filter_upsample_accumulate(__read_only image2d_t coarse,
                                                   float coarse_weight,
                                                   __read_only image2d_t level,
                                                   __write_only image2d_t dst, int width,
                                                   int height, float level_weight) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) {
    return;
  }
  const float  cx      = ((float)x + 0.5f) * 0.5f;
  const float  cy      = ((float)y + 0.5f) * 0.5f;
  const float4 center  = DiffusionBilinear(coarse, cx, cy);
  const float4 edges   = DiffusionBilinear(coarse, cx - 1.0f, cy) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy) +
                         DiffusionBilinear(coarse, cx, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx, cy + 1.0f);
  const float4 corners = DiffusionBilinear(coarse, cx - 1.0f, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy - 1.0f) +
                         DiffusionBilinear(coarse, cx - 1.0f, cy + 1.0f) +
                         DiffusionBilinear(coarse, cx + 1.0f, cy + 1.0f);
  const float4 tent    = (center * 4.0f + edges * 2.0f + corners) * (1.0f / 16.0f);
  const float4 own     = read_imagef(level, kNearestClamp, (int2)(x, y));
  write_imagef(dst, (int2)(x, y), own * level_weight + tent * coarse_weight);
}

/// out = T * ((1 - s) * I + s * B); B is the scatter image at the reference position of the
/// render pixel, so every render of the same frame reads the same glow.
__kernel void diffusion_filter_mix_scene_rgba32f(__read_only image2d_t src_image,
                                                 __global const float4* src_buffer,
                                                 int src_is_buffer,
                                                 __write_only image2d_t dst_image,
                                                 __global float4* dst_buffer, int dst_is_buffer,
                                                 __read_only image2d_t scatter,
                                                 DiffusionMixParams params) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= params.width || y >= params.height) {
    return;
  }
  const int    index  = y * params.width + x;
  const float4 source = src_is_buffer != 0 ? src_buffer[index]
                                           : read_imagef(src_image, kNearestClamp, (int2)(x, y));
  const float4 linear = DiffusionDecode(source);
  const float2 base =
      DiffusionTransform(params.render_to_base, (float)x + 0.5f, (float)y + 0.5f);
  const float4 glow   = DiffusionSampleBSpline(scatter, base.x, base.y);
  const float  direct = 1.0f - params.scatter_fraction;
  const float  t      = params.transmission;
  const float  s      = params.scatter_fraction;
  const float4 outp   = (float4)(t * (direct * linear.x + s * glow.x),
                                 t * (direct * linear.y + s * glow.y),
                                 t * (direct * linear.z + s * glow.z), linear.w);
  if (dst_is_buffer != 0) {
    dst_buffer[index] = outp;
  } else {
    write_imagef(dst_image, (int2)(x, y), outp);
  }
}
