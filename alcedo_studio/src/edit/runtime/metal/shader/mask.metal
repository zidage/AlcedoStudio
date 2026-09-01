//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct MaskSampleParams {
  float render_to_uv[9];
  uint  source_width;
  uint  source_height;
  uint  output_width;
  uint  output_height;
  uint  invert;
  float opacity;
  uint  pad1;
};

struct MaskFeatherParams {
  float render_to_uv[9];
  uint  source_width;
  uint  source_height;
  uint  output_width;
  uint  output_height;
  float radius_texels;
  uint  invert;
  float opacity;
};

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

struct MaskBandParams {
  uint width;
  uint height;
  uint target_inside;
  uint pad;
};

struct MaskMipParams {
  uint source_width;
  uint source_height;
  uint destination_width;
  uint destination_height;
};

static inline float2 Transform(constant float m[9], float x, float y) {
  return float2(m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]);
}

static inline float SampleR8(texture2d<float, access::read> pixels, uint width, uint height,
                             float u, float v) {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return 0.0f;
  }
  const float x  = u * static_cast<float>(width) - 0.5f;
  const float y  = v * static_cast<float>(height) - 0.5f;
  const int   x0 = max(0, min(static_cast<int>(width) - 1, static_cast<int>(floor(x))));
  const int   y0 = max(0, min(static_cast<int>(height) - 1, static_cast<int>(floor(y))));
  const int   x1 = min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = min(max(x - floor(x), 0.0f), 1.0f);
  const float ty = min(max(y - floor(y), 0.0f), 1.0f);
  const float a =
      pixels.read(uint2(static_cast<uint>(x0), static_cast<uint>(y0))).r * (1.0f - tx) +
      pixels.read(uint2(static_cast<uint>(x1), static_cast<uint>(y0))).r * tx;
  const float b =
      pixels.read(uint2(static_cast<uint>(x0), static_cast<uint>(y1))).r * (1.0f - tx) +
      pixels.read(uint2(static_cast<uint>(x1), static_cast<uint>(y1))).r * tx;
  return a * (1.0f - ty) + b * ty;
}

static inline float SampleF32(device const float* pixels, uint width, uint height, float u,
                              float v) {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return -1.0e6f;
  }
  const float x  = u * static_cast<float>(width) - 0.5f;
  const float y  = v * static_cast<float>(height) - 0.5f;
  const int   x0 = max(0, min(static_cast<int>(width) - 1, static_cast<int>(floor(x))));
  const int   y0 = max(0, min(static_cast<int>(height) - 1, static_cast<int>(floor(y))));
  const int   x1 = min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = min(max(x - floor(x), 0.0f), 1.0f);
  const float ty = min(max(y - floor(y), 0.0f), 1.0f);
  const float a  = pixels[static_cast<uint>(y0) * width + static_cast<uint>(x0)] * (1.0f - tx) +
                  pixels[static_cast<uint>(y0) * width + static_cast<uint>(x1)] * tx;
  const float b = pixels[static_cast<uint>(y1) * width + static_cast<uint>(x0)] * (1.0f - tx) +
                  pixels[static_cast<uint>(y1) * width + static_cast<uint>(x1)] * tx;
  return a * (1.0f - ty) + b * ty;
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

kernel void mask_generate_r8_mip(texture2d<float, access::read> source [[texture(0)]],
                                 texture2d<float, access::write> destination [[texture(1)]],
                                 constant MaskMipParams& params [[buffer(0)]],
                                 uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.destination_width || gid.y >= params.destination_height) {
    return;
  }
  const uint source_x = gid.x * 2u;
  const uint source_y = gid.y * 2u;
  uint       sum      = 0;
  uint       count    = 0;
  for (uint dy = 0; dy < 2u; ++dy) {
    for (uint dx = 0; dx < 2u; ++dx) {
      const uint sx = source_x + dx;
      const uint sy = source_y + dy;
      if (sx < params.source_width && sy < params.source_height) {
        sum += static_cast<uint>(source.read(uint2(sx, sy)).r * 255.0f + 0.5f);
        ++count;
      }
    }
  }
  const float value = count == 0 ? 0.0f : static_cast<float>((sum + count / 2u) / count) / 255.0f;
  destination.write(float4(value, 0.0f, 0.0f, 1.0f), gid);
}

kernel void mask_raster_sample(texture2d<float, access::read> source [[texture(0)]],
                               texture2d<float, access::write> output [[texture(1)]],
                               constant MaskSampleParams& params [[buffer(0)]],
                               uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.output_width || gid.y >= params.output_height) {
    return;
  }
  const float2 uv    = Transform(params.render_to_uv, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  float        value = SampleR8(source, params.source_width, params.source_height, uv.x, uv.y);
  output.write(float4(QuantizeR8(FinishEffectiveCoverage(value, params.invert, params.opacity)),
                      0.0f, 0.0f, 1.0f),
               gid);
}

kernel void mask_band_horizontal(texture2d<float, access::read> source [[texture(0)]],
                                 device float* squared_distance [[buffer(0)]],
                                 constant MaskBandParams& params [[buffer(1)]],
                                 uint y [[thread_position_in_grid]]) {
  if (y >= params.height) {
    return;
  }
  constexpr int missing = -100000;
  const bool    want    = params.target_inside != 0u;
  int           nearest = missing;
  for (uint x = 0; x < params.width; ++x) {
    const bool  inside = source.read(uint2(x, y)).r >= 0.5f;
    if (inside == want) {
      nearest = static_cast<int>(x);
    }
    const float dx = static_cast<float>(static_cast<int>(x) - nearest);
    squared_distance[y * params.width + x] = nearest == missing ? 1.0e20f : dx * dx;
  }
  nearest = -missing;
  for (int x = static_cast<int>(params.width) - 1; x >= 0; --x) {
    const bool inside = source.read(uint2(static_cast<uint>(x), y)).r >= 0.5f;
    if (inside == want) {
      nearest = x;
    }
    if (nearest != -missing) {
      const float dx = static_cast<float>(x - nearest);
      const uint  i  = y * params.width + static_cast<uint>(x);
      squared_distance[i] = min(squared_distance[i], dx * dx);
    }
  }
}

kernel void mask_band_vertical(device const float* horizontal [[buffer(0)]],
                               device float* squared_distance [[buffer(1)]],
                               device int* sites [[buffer(2)]],
                               device float* boundaries [[buffer(3)]],
                               constant MaskBandParams& params [[buffer(4)]],
                               uint x [[thread_position_in_grid]]) {
  if (x >= params.width) {
    return;
  }
  const uint base = x * params.height;
  int        count = 0;
  for (uint y = 0; y < params.height; ++y) {
    const float f = horizontal[y * params.width + x];
    if (f >= 1.0e19f) {
      continue;
    }
    float boundary = -1.0e20f;
    while (count > 0) {
      const int   previous   = sites[base + static_cast<uint>(count - 1)];
      const float previous_f = horizontal[static_cast<uint>(previous) * params.width + x];
      boundary               = ((f + static_cast<float>(y * y)) -
                  (previous_f + static_cast<float>(previous * previous))) /
                 (2.0f * (static_cast<float>(y) - static_cast<float>(previous)));
      if (boundary > boundaries[base + static_cast<uint>(count - 1)]) {
        break;
      }
      --count;
    }
    sites[base + static_cast<uint>(count)]      = static_cast<int>(y);
    boundaries[base + static_cast<uint>(count)] = count == 0 ? -1.0e20f : boundary;
    ++count;
  }
  if (count == 0) {
    for (uint y = 0; y < params.height; ++y) {
      squared_distance[y * params.width + x] = 1.0e20f;
    }
    return;
  }
  int site_index = 0;
  for (uint y = 0; y < params.height; ++y) {
    while (site_index + 1 < count &&
           boundaries[base + static_cast<uint>(site_index + 1)] < static_cast<float>(y)) {
      ++site_index;
    }
    const int   site = sites[base + static_cast<uint>(site_index)];
    const float dy   = static_cast<float>(static_cast<int>(y) - site);
    squared_distance[y * params.width + x] =
        horizontal[static_cast<uint>(site) * params.width + x] + dy * dy;
  }
}

kernel void mask_compose_signed_distance(texture2d<float, access::read> source [[texture(0)]],
                                         device const float* distance_to_inside [[buffer(0)]],
                                         device const float* distance_to_outside [[buffer(1)]],
                                         device float* distance [[buffer(2)]],
                                         constant MaskBandParams& params [[buffer(3)]],
                                         uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }
  const uint  index    = gid.y * params.width + gid.x;
  const float coverage = source.read(gid).r;
  const bool  inside   = coverage >= 0.5f;
  const float exact    = sqrt(inside ? distance_to_outside[index] : distance_to_inside[index]);
  if (coverage > 0.0f && coverage < 1.0f) {
    distance[index] = coverage - 0.5f;
  } else {
    const float to_boundary = max(exact - 0.5f, 0.0f);
    distance[index]         = inside ? to_boundary : -to_boundary;
  }
}

kernel void mask_feather_sample(device const float* distance [[buffer(0)]],
                                texture2d<float, access::write> output [[texture(0)]],
                                constant MaskFeatherParams& params [[buffer(1)]],
                                uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.output_width || gid.y >= params.output_height) {
    return;
  }
  const float2 uv = Transform(params.render_to_uv, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const float  d  = SampleF32(distance, params.source_width, params.source_height, uv.x, uv.y);
  float        value =
      params.radius_texels <= 0.0f ? (d >= 0.0f ? 1.0f : 0.0f)
                                   : min(max(0.5f + d / (2.0f * params.radius_texels), 0.0f), 1.0f);
  value = value * value * (3.0f - 2.0f * value);
  output.write(float4(QuantizeR8(FinishEffectiveCoverage(value, params.invert, params.opacity)),
                      0.0f, 0.0f, 1.0f),
               gid);
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
