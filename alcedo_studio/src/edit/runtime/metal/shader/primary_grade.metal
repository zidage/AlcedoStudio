//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct GradeAdjustmentParams {
  uint  behavior;
  uint  count;
  float values[30];
};

struct PrimaryGradeDispatchParams {
  uint  command_count;
  uint  command_offset;
  uint  lut_edge;
  float local_reference;
  uint  width;
  uint  pad[3];
};

static inline float Luma(float3 c) {
  return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z;
}

static inline float ExtrapolateCurve(float value, device const GradeAdjustmentParams& p, uint a,
                                     uint b) {
  const float x0 = p.values[a * 2];
  const float y0 = p.values[a * 2 + 1];
  const float x1 = p.values[b * 2];
  const float y1 = p.values[b * 2 + 1];
  return y0 + (value - x0) * (y1 - y0) / max(x1 - x0, 1.0e-6f);
}

static inline float ApplyCurve(float value, device const GradeAdjustmentParams& p) {
  if (p.count < 2) {
    return value;
  }
  if (value <= p.values[0]) {
    return ExtrapolateCurve(value, p, 0, 1);
  }
  for (uint i = 1; i < p.count; ++i) {
    const float x1 = p.values[i * 2];
    if (value <= x1) {
      const float x0 = p.values[(i - 1) * 2];
      const float y0 = p.values[(i - 1) * 2 + 1];
      const float y1 = p.values[i * 2 + 1];
      const float t  = (value - x0) / max(x1 - x0, 1.0e-6f);
      return y0 + t * (y1 - y0);
    }
  }
  return ExtrapolateCurve(value, p, p.count - 2, p.count - 1);
}

static inline float3 ApplyHls(float3 c, device const GradeAdjustmentParams& p) {
  const float maximum = max(c.x, max(c.y, c.z));
  const float minimum = min(c.x, min(c.y, c.z));
  const float chroma  = maximum - minimum;
  float       hue     = 0.0f;
  if (chroma > 1.0e-6f) {
    if (maximum == c.x) {
      hue = 60.0f * fmod((c.y - c.z) / chroma, 6.0f);
    } else if (maximum == c.y) {
      hue = 60.0f * ((c.z - c.x) / chroma + 2.0f);
    } else {
      hue = 60.0f * ((c.x - c.y) / chroma + 4.0f);
    }
  }
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  const int   bin        = int((hue + 22.5f) / 45.0f) & 7;
  const float luma       = Luma(c);
  const float saturation = 1.0f + p.values[16 + bin];
  c.x                    = luma + (c.x - luma) * saturation;
  c.y                    = luma + (c.y - luma) * saturation;
  c.z                    = luma + (c.z - luma) * saturation;
  const float lightness  = p.values[8 + bin];
  c.x += lightness;
  c.y += lightness;
  c.z += lightness;
  return c;
}

static inline uint LutIndex(uint edge, uint x, uint y, uint z) { return (z * edge + y) * edge + x; }

static inline float3 SampleLut3d(device const float4* lut, uint edge, float u, float v, float w) {
  if (lut == nullptr || edge <= 1) {
    return float3(u, v, w);
  }
  const float3 coord   = clamp(float3(u, v, w), 0.0f, 1.0f);
  const float3 tex_pos = coord * float(edge) - 0.5f;
  const float3 pos     = clamp(tex_pos, 0.0f, float(edge - 1));
  const uint3  lo      = uint3(pos);
  const uint3  hi      = min(lo + uint3(1), uint3(edge - 1));
  const float3 t       = pos - float3(lo);
  const float4 c000    = lut[LutIndex(edge, lo.x, lo.y, lo.z)];
  const float4 c100    = lut[LutIndex(edge, hi.x, lo.y, lo.z)];
  const float4 c010    = lut[LutIndex(edge, lo.x, hi.y, lo.z)];
  const float4 c110    = lut[LutIndex(edge, hi.x, hi.y, lo.z)];
  const float4 c001    = lut[LutIndex(edge, lo.x, lo.y, hi.z)];
  const float4 c101    = lut[LutIndex(edge, hi.x, lo.y, hi.z)];
  const float4 c011    = lut[LutIndex(edge, lo.x, hi.y, hi.z)];
  const float4 c111    = lut[LutIndex(edge, hi.x, hi.y, hi.z)];
  const float4 c00     = mix(c000, c100, t.x);
  const float4 c10     = mix(c010, c110, t.x);
  const float4 c01     = mix(c001, c101, t.x);
  const float4 c11     = mix(c011, c111, t.x);
  const float4 c0      = mix(c00, c10, t.y);
  const float4 c1      = mix(c01, c11, t.y);
  const float4 sampled = mix(c0, c1, t.z);
  return sampled.xyz;
}

static inline float3 ApplyAdjustment(float3 c, device const GradeAdjustmentParams& p,
                                     uint pixel_index, float local_reference,
                                     device const float4* lut, uint lut_edge) {
  const uint  behavior = p.behavior;
  const float value    = p.values[0];
  if (behavior == 0u && value != 0.0f) {
    const float temperature = p.values[1] * 0.001f;
    const float tint        = p.values[2] * 0.001f;
    c.x *= exp2(temperature - tint * 0.5f);
    c.y *= exp2(tint);
    c.z *= exp2(-temperature - tint * 0.5f);
  } else if (behavior == 1u) {
    const float offset = value / 17.52f;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (behavior == 2u) {
    const float scale = 1.0f + value * 0.01f;
    c.x               = (c.x - 0.18f) * scale + 0.18f;
    c.y               = (c.y - 0.18f) * scale + 0.18f;
    c.z               = (c.z - 0.18f) * scale + 0.18f;
  } else if (behavior == 3u) {
    const float gain = 1.0f + max(value, 0.0f) * 0.005f;
    c.x *= gain;
    c.y *= gain;
    c.z *= gain;
  } else if (behavior == 4u) {
    const float offset = value * 0.001f;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (behavior == 13u) {
    const float l      = Luma(c);
    float       weight = 1.0f - min(l / max(local_reference, 1.0e-4f), 1.0f);
    weight             = 0.5f - fabs(weight - 0.5f);
    const float gain   = 1.0f + value * 0.01f * weight;
    c.x *= gain;
    c.y *= gain;
    c.z *= gain;
  } else if (behavior == 7u) {
    c.x = ApplyCurve(c.x, p);
    c.y = ApplyCurve(c.y, p);
    c.z = ApplyCurve(c.z, p);
  } else if (behavior == 8u) {
    c = ApplyHls(c, p);
  } else if (behavior == 9u || behavior == 10u) {
    const float l = Luma(c);
    float scale   = behavior == 9u ? value : 1.0f + value * 0.01f;
    if (behavior == 10u) {
      const float maximum = max(c.x, max(c.y, c.z));
      const float minimum = min(c.x, min(c.y, c.z));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - min(maximum - minimum, 1.0f));
    }
    c.x = l + (c.x - l) * scale;
    c.y = l + (c.y - l) * scale;
    c.z = l + (c.z - l) * scale;
  } else if (behavior == 11u) {
    const float gamma_x = max(p.values[4] + p.values[7], 1.0e-4f);
    const float gamma_y = max(p.values[5] + p.values[7], 1.0e-4f);
    const float gamma_z = max(p.values[6] + p.values[7], 1.0e-4f);
    c.x = copysign(pow(fabs(c.x + p.values[0] + p.values[3]), 1.0f / gamma_x), c.x) * p.values[8];
    c.y = copysign(pow(fabs(c.y + p.values[1] + p.values[3]), 1.0f / gamma_y), c.y) * p.values[9];
    c.z = copysign(pow(fabs(c.z + p.values[2] + p.values[3]), 1.0f / gamma_z), c.z) * p.values[10];
  } else if (behavior == 14u) {
    const float l     = Luma(c);
    const float scale = 1.0f + value * 0.0025f;
    c.x               = l + (c.x - l) * scale;
    c.y               = l + (c.y - l) * scale;
    c.z               = l + (c.z - l) * scale;
  } else if (behavior == 15u) {
    c.x += max(Luma(c) - 0.6f, 0.0f) * value * 0.15f;
  } else if (behavior == 16u && value != 0.0f) {
    uint        hash  = pixel_index * 747796405u + 2891336453u;
    hash              = (hash >> ((hash >> 28u) + 4u)) ^ hash;
    const float noise = (float(hash & 0xffffu) / 32767.5f - 1.0f) * value * 0.02f;
    c.x += noise;
    c.y += noise;
    c.z += noise;
  } else if (behavior == 12u && value != 0.0f && lut_edge > 1u) {
    const float scale  = float(lut_edge - 1u) / float(lut_edge);
    const float offset = 1.0f / (2.0f * float(lut_edge));
    c = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset, c.z * scale + offset);
  }
  return c;
}

static inline device const GradeAdjustmentParams& LoadParams(device const uchar* parameter_base,
                                                             uint offset) {
  device const GradeAdjustmentParams* params =
      (device const GradeAdjustmentParams*)(parameter_base + offset);
  return *params;
}

kernel void primary_grade_pointwise(texture2d<float, access::read> src [[texture(0)]],
                                    texture2d<float, access::write> dst [[texture(1)]],
                                    device const uchar* parameter_base [[buffer(0)]],
                                    device const uint* commands [[buffer(1)]],
                                    constant PrimaryGradeDispatchParams& dispatch [[buffer(2)]],
                                    device const float4* lmt_lut [[buffer(3)]],
                                    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= src.get_width() || gid.y >= src.get_height()) {
    return;
  }
  const float4 source      = src.read(gid);
  float3       c           = source.xyz;
  const uint   pixel_index = gid.y * dispatch.width + gid.x;
  for (uint i = 0; i < dispatch.command_count; ++i) {
    const uint offset = commands[dispatch.command_offset + i];
    c = ApplyAdjustment(c, LoadParams(parameter_base, offset), pixel_index, dispatch.local_reference,
                        lmt_lut, dispatch.lut_edge);
  }
  dst.write(float4(c, source.w), gid);
}

kernel void primary_grade_mix(texture2d<float, access::read> source [[texture(0)]],
                              texture2d<float, access::read> adjusted [[texture(1)]],
                              texture2d<float, access::write> dst [[texture(2)]],
                              constant float& grade_mix [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= source.get_width() || gid.y >= source.get_height()) {
    return;
  }
  const float4 a = adjusted.read(gid);
  const float4 s = source.read(gid);
  dst.write(float4(s.xyz + (a.xyz - s.xyz) * grade_mix, s.w), gid);
}
