//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

typedef struct {
  float render_to_uv[9];
  uint  source_width;
  uint  source_height;
  uint  output_width;
  uint  output_height;
  uint  invert;
  uint  pad0;
  uint  pad1;
} MaskSampleParams;

typedef struct {
  float render_to_uv[9];
  uint  source_width;
  uint  source_height;
  uint  output_width;
  uint  output_height;
  float radius_texels;
  uint  invert;
  uint  pad0;
} MaskFeatherParams;

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
  uint  graduated_invert;
} MaskAnalyticParams;

typedef struct {
  uint width;
  uint height;
  uint target_inside;
  uint pad;
} MaskBandParams;

typedef struct {
  uint source_width;
  uint source_height;
  uint destination_width;
  uint destination_height;
} MaskMipParams;

static inline float2 Transform(__private const float* matrix, float x, float y) {
  return (float2)(matrix[0] * x + matrix[1] * y + matrix[2],
                  matrix[3] * x + matrix[4] * y + matrix[5]);
}

static inline float SampleR8(__read_only image2d_t pixels, uint width, uint height, float u,
                             float v) {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return 0.0f;
  }
  const float x  = u * (float)width - 0.5f;
  const float y  = v * (float)height - 0.5f;
  const int   x0 = max(0, min((int)width - 1, (int)floor(x)));
  const int   y0 = max(0, min((int)height - 1, (int)floor(y)));
  const int   x1 = min(x0 + 1, (int)width - 1);
  const int   y1 = min(y0 + 1, (int)height - 1);
  const float tx = min(max(x - floor(x), 0.0f), 1.0f);
  const float ty = min(max(y - floor(y), 0.0f), 1.0f);
  const float a  = read_imagef(pixels, kNearestClamp, (int2)(x0, y0)).x * (1.0f - tx) +
                  read_imagef(pixels, kNearestClamp, (int2)(x1, y0)).x * tx;
  const float b  = read_imagef(pixels, kNearestClamp, (int2)(x0, y1)).x * (1.0f - tx) +
                  read_imagef(pixels, kNearestClamp, (int2)(x1, y1)).x * tx;
  return a * (1.0f - ty) + b * ty;
}

static inline float SampleF32(__global const float* pixels, uint offset, uint width, uint height,
                              float u, float v) {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return -1.0e6f;
  }
  const float x  = u * (float)width - 0.5f;
  const float y  = v * (float)height - 0.5f;
  const int   x0 = max(0, min((int)width - 1, (int)floor(x)));
  const int   y0 = max(0, min((int)height - 1, (int)floor(y)));
  const int   x1 = min(x0 + 1, (int)width - 1);
  const int   y1 = min(y0 + 1, (int)height - 1);
  const float tx = min(max(x - floor(x), 0.0f), 1.0f);
  const float ty = min(max(y - floor(y), 0.0f), 1.0f);
  const float a  = pixels[offset + (uint)y0 * width + (uint)x0] * (1.0f - tx) +
                  pixels[offset + (uint)y0 * width + (uint)x1] * tx;
  const float b  = pixels[offset + (uint)y1 * width + (uint)x0] * (1.0f - tx) +
                  pixels[offset + (uint)y1 * width + (uint)x1] * tx;
  return a * (1.0f - ty) + b * ty;
}

static inline float QuantizeR8(float value) {
  return (float)((uint)min(max(value * 255.0f + 0.5f, 0.0f), 255.0f)) / 255.0f;
}

__kernel void mask_generate_r8_mip(__read_only image2d_t source, __write_only image2d_t destination,
                                   MaskMipParams params) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.destination_width || gid.y >= params.destination_height) {
    return;
  }
  const uint source_x = gid.x * 2u;
  const uint source_y = gid.y * 2u;
  uint       sum      = 0u;
  uint       count    = 0u;
  for (uint dy = 0u; dy < 2u; ++dy) {
    for (uint dx = 0u; dx < 2u; ++dx) {
      const uint sx = source_x + dx;
      const uint sy = source_y + dy;
      if (sx < params.source_width && sy < params.source_height) {
        sum += (uint)(read_imagef(source, kNearestClamp, (int2)(sx, sy)).x * 255.0f + 0.5f);
        ++count;
      }
    }
  }
  const float value = count == 0u ? 0.0f : (float)((sum + count / 2u) / count) / 255.0f;
  write_imagef(destination, (int2)(gid.x, gid.y), (float4)(value, 0.0f, 0.0f, 1.0f));
}

__kernel void mask_raster_sample_r8(__read_only image2d_t source, __write_only image2d_t output,
                                    MaskSampleParams params) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.output_width || gid.y >= params.output_height) {
    return;
  }
  const float2 uv = Transform(params.render_to_uv, (float)gid.x + 0.5f, (float)gid.y + 0.5f);
  float        value = SampleR8(source, params.source_width, params.source_height, uv.x, uv.y);
  if (params.invert != 0u) {
    value = 1.0f - value;
  }
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(QuantizeR8(value), 0.0f, 0.0f, 1.0f));
}

__kernel void mask_band_horizontal(__read_only image2d_t source,
                                   __global float* squared_distance, MaskBandParams params,
                                   uint output_offset) {
  const uint y = get_global_id(0);
  if (y >= params.height) {
    return;
  }
  const bool want    = params.target_inside != 0u;
  int        nearest = -1;
  for (uint x = 0u; x < params.width; ++x) {
    const bool inside = read_imagef(source, kNearestClamp, (int2)(x, y)).x >= 0.5f;
    if (inside == want) {
      nearest = (int)x;
    }
    const float dx = (float)((int)x - nearest);
    squared_distance[output_offset + y * params.width + x] = nearest < 0 ? 1.0e20f : dx * dx;
  }
  nearest = (int)params.width;
  for (int x = (int)params.width - 1; x >= 0; --x) {
    const bool inside = read_imagef(source, kNearestClamp, (int2)(x, (int)y)).x >= 0.5f;
    if (inside == want) {
      nearest = x;
    }
    if (nearest < (int)params.width) {
      const float dx = (float)(x - nearest);
      const uint  i  = y * params.width + (uint)x;
      squared_distance[output_offset + i] =
          min(squared_distance[output_offset + i], dx * dx);
    }
  }
}

__kernel void mask_band_vertical(__global const float* horizontal, __global float* squared_distance,
                                 __global int* sites, __global float* boundaries,
                                 MaskBandParams params, uint horizontal_offset,
                                 uint distance_offset, uint sites_offset, uint boundaries_offset) {
  const uint x = get_global_id(0);
  if (x >= params.width) {
    return;
  }
  const uint site_base     = sites_offset + x * params.height;
  const uint boundary_base = boundaries_offset + x * params.height;
  int        count         = 0;
  for (uint y = 0u; y < params.height; ++y) {
    const float f = horizontal[horizontal_offset + y * params.width + x];
    if (f >= 1.0e19f) {
      continue;
    }
    float boundary = -1.0e20f;
    while (count > 0) {
      const int   previous = sites[site_base + (uint)(count - 1)];
      const float previous_f = horizontal[horizontal_offset + (uint)previous * params.width + x];
      boundary = ((f + (float)(y * y)) -
                  (previous_f + (float)(previous * previous))) /
                 (2.0f * ((float)y - (float)previous));
      if (boundary > boundaries[boundary_base + (uint)(count - 1)]) {
        break;
      }
      --count;
    }
    sites[site_base + (uint)count] = (int)y;
    boundaries[boundary_base + (uint)count] = count == 0 ? -1.0e20f : boundary;
    ++count;
  }
  if (count == 0) {
    for (uint y = 0u; y < params.height; ++y) {
      squared_distance[distance_offset + y * params.width + x] = 1.0e20f;
    }
    return;
  }
  int site_index = 0;
  for (uint y = 0u; y < params.height; ++y) {
    while (site_index + 1 < count &&
           boundaries[boundary_base + (uint)(site_index + 1)] < (float)y) {
      ++site_index;
    }
    const int   site = sites[site_base + (uint)site_index];
    const float dy   = (float)((int)y - site);
    squared_distance[distance_offset + y * params.width + x] =
        horizontal[horizontal_offset + (uint)site * params.width + x] + dy * dy;
  }
}

__kernel void mask_compose_signed_distance(__read_only image2d_t source,
                                           __global const float* distance_to_inside,
                                           __global const float* distance_to_outside,
                                           __global float* distance, MaskBandParams params,
                                           uint inside_offset, uint outside_offset,
                                           uint distance_offset) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint  index    = gid.y * params.width + gid.x;
  const float coverage = read_imagef(source, kNearestClamp, (int2)(gid.x, gid.y)).x;
  const bool  inside   = coverage >= 0.5f;
  const float exact    = sqrt(inside ? distance_to_outside[outside_offset + index]
                                    : distance_to_inside[inside_offset + index]);
  if (coverage > 0.0f && coverage < 1.0f) {
    distance[distance_offset + index] = coverage - 0.5f;
  } else {
    const float to_boundary = max(exact - 0.5f, 0.0f);
    distance[distance_offset + index] = inside ? to_boundary : -to_boundary;
  }
}

__kernel void mask_feather_sample(__global const float* distance, __write_only image2d_t output,
                                  MaskFeatherParams params, uint distance_offset) {
  const uint2 gid = (uint2)(get_global_id(0), get_global_id(1));
  if (gid.x >= params.output_width || gid.y >= params.output_height) {
    return;
  }
  const float2 uv = Transform(params.render_to_uv, (float)gid.x + 0.5f, (float)gid.y + 0.5f);
  const float  d  = SampleF32(distance, distance_offset, params.source_width,
                              params.source_height, uv.x, uv.y);
  float value = params.radius_texels <= 0.0f
                    ? (d >= 0.0f ? 1.0f : 0.0f)
                    : min(max(0.5f + d / (2.0f * params.radius_texels), 0.0f), 1.0f);
  value = value * value * (3.0f - 2.0f * value);
  if (params.invert != 0u) {
    value = 1.0f - value;
  }
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(QuantizeR8(value), 0.0f, 0.0f, 1.0f));
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
    invert = params.graduated_invert;
  }
  if (invert != 0u) {
    value = 1.0f - value;
  }
  write_imagef(output, (int2)(gid.x, gid.y), (float4)(QuantizeR8(value), 0.0f, 0.0f, 1.0f));
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
