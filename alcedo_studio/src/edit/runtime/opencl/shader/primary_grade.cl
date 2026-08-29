//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

__constant sampler_t kNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

typedef struct {
  uint  behavior;
  uint  count;
  float values[30];
} GradeAdjustmentParams;

typedef struct {
  uint  command_count;
  uint  command_offset;
  uint  lut_edge;
  float local_reference;
  uint  width;
  uint  pad[3];
} PrimaryGradeDispatchParams;

static inline float Luma(float3 c) { return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z; }

static inline float ExtrapolateCurve(float value, __global const GradeAdjustmentParams* p, uint a,
                                     uint b) {
  const float x0 = p->values[a * 2];
  const float y0 = p->values[a * 2 + 1];
  const float x1 = p->values[b * 2];
  const float y1 = p->values[b * 2 + 1];
  return y0 + (value - x0) * (y1 - y0) / max(x1 - x0, 1.0e-6f);
}

static inline float ApplyCurve(float value, __global const GradeAdjustmentParams* p) {
  if (p->count < 2u) {
    return value;
  }
  if (value <= p->values[0]) {
    return ExtrapolateCurve(value, p, 0u, 1u);
  }
  for (uint i = 1u; i < p->count; ++i) {
    const float x1 = p->values[i * 2u];
    if (value <= x1) {
      const float x0 = p->values[(i - 1u) * 2u];
      const float y0 = p->values[(i - 1u) * 2u + 1u];
      const float y1 = p->values[i * 2u + 1u];
      const float t  = (value - x0) / max(x1 - x0, 1.0e-6f);
      return y0 + t * (y1 - y0);
    }
  }
  return ExtrapolateCurve(value, p, p->count - 2u, p->count - 1u);
}

static inline float3 ApplyHls(float3 c, __global const GradeAdjustmentParams* p) {
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
  const int   bin        = ((int)((hue + 22.5f) / 45.0f)) & 7;
  const float luma       = Luma(c);
  const float saturation = 1.0f + p->values[16 + bin];
  c.x                    = luma + (c.x - luma) * saturation;
  c.y                    = luma + (c.y - luma) * saturation;
  c.z                    = luma + (c.z - luma) * saturation;
  const float lightness  = p->values[8 + bin];
  c.x += lightness;
  c.y += lightness;
  c.z += lightness;
  return c;
}

static inline uint LutIndex(uint edge, uint x, uint y, uint z) { return (z * edge + y) * edge + x; }

static inline float3 SampleLut3d(__global const float4* lut, uint edge, float u, float v, float w) {
  const float3 coord   = clamp((float3)(u, v, w), 0.0f, 1.0f);
  const float3 tex_pos = coord * (float)edge - 0.5f;
  const float3 pos     = clamp(tex_pos, 0.0f, (float)(edge - 1u));
  const uint3  lo      = convert_uint3(pos);
  const uint3  hi      = min(lo + (uint3)(1u), (uint3)(edge - 1u));
  const float3 t       = pos - convert_float3(lo);
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

static inline float3 ApplyAdjustment(float3 c, __global const GradeAdjustmentParams* p,
                                     __global const float4* lut, uint lut_edge) {
  const uint  behavior = p->behavior;
  const float value    = p->values[0];
  if (behavior == 0u && value != 0.0f) {
    const float temperature = p->values[1] * 0.001f;
    const float tint        = p->values[2] * 0.001f;
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
  } else if (behavior == 7u) {
    c.x = ApplyCurve(c.x, p);
    c.y = ApplyCurve(c.y, p);
    c.z = ApplyCurve(c.z, p);
  } else if (behavior == 8u) {
    c = ApplyHls(c, p);
  } else if (behavior == 9u || behavior == 10u) {
    const float l     = Luma(c);
    float       scale = behavior == 9u ? value : 1.0f + value * 0.01f;
    if (behavior == 10u) {
      const float maximum = max(c.x, max(c.y, c.z));
      const float minimum = min(c.x, min(c.y, c.z));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - min(maximum - minimum, 1.0f));
    }
    c.x = l + (c.x - l) * scale;
    c.y = l + (c.y - l) * scale;
    c.z = l + (c.z - l) * scale;
  } else if (behavior == 11u) {
    const float gamma_x = max(p->values[4] + p->values[7], 1.0e-4f);
    const float gamma_y = max(p->values[5] + p->values[7], 1.0e-4f);
    const float gamma_z = max(p->values[6] + p->values[7], 1.0e-4f);
    c.x =
        copysign(pow(fabs(c.x + p->values[0] + p->values[3]), 1.0f / gamma_x), c.x) * p->values[8];
    c.y =
        copysign(pow(fabs(c.y + p->values[1] + p->values[3]), 1.0f / gamma_y), c.y) * p->values[9];
    c.z =
        copysign(pow(fabs(c.z + p->values[2] + p->values[3]), 1.0f / gamma_z), c.z) * p->values[10];
  } else if (behavior == 12u && value != 0.0f && lut_edge > 1u) {
    const float scale  = (float)(lut_edge - 1u) / (float)lut_edge;
    const float offset = 1.0f / (2.0f * (float)lut_edge);
    c                  = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset,
                                     c.z * scale + offset);
  }
  return c;
}

static inline float4 GradePixel(read_only image2d_t src, __global const uchar* parameter_base,
                                __global const uint* commands, PrimaryGradeDispatchParams dispatch,
                                int2 gid, __global const float4* lut) {
  const float4 source = read_imagef(src, kNearestClamp, gid);
  float3       c      = source.xyz;
  for (uint i = 0u; i < dispatch.command_count; ++i) {
    const uint                            offset = commands[dispatch.command_offset + i];
    __global const GradeAdjustmentParams* params =
        (__global const GradeAdjustmentParams*)(parameter_base + offset);
    c = ApplyAdjustment(c, params, lut, dispatch.lut_edge);
  }
  return (float4)(c.x, c.y, c.z, source.w);
}

static inline void WriteGradePixel(read_only image2d_t src, write_only image2d_t dst,
                                   __global const uchar*      parameter_base,
                                   __global const uint*       commands,
                                   PrimaryGradeDispatchParams dispatch, __global const float4* lut,
                                   int2 gid) {
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  write_imagef(dst, gid, GradePixel(src, parameter_base, commands, dispatch, gid, lut));
}

__kernel void primary_grade_pointwise_rgba32f(__read_only image2d_t src, __write_only image2d_t dst,
                                              __global const uchar*      parameter_base,
                                              __global const uint*       commands,
                                              PrimaryGradeDispatchParams dispatch,
                                              __global const float4*     lut) {
  const int2 gid = (int2)((int)get_global_id(0), (int)get_global_id(1));
  WriteGradePixel(src, dst, parameter_base, commands, dispatch, lut, gid);
}

__kernel void primary_grade_mix_rgba32f(__read_only image2d_t source,
                                        __read_only image2d_t adjusted, __write_only image2d_t dst,
                                        float grade_mix) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  const float4 a   = read_imagef(adjusted, kNearestClamp, gid);
  const float4 s   = read_imagef(source, kNearestClamp, gid);
  const float  mix = clamp(grade_mix, 0.0f, 1.0f);
  write_imagef(dst, gid, (float4)(s.xyz + (a.xyz - s.xyz) * mix, s.w));
}

__kernel void primary_grade_mix_masked_rgba32f(__read_only image2d_t  source,
                                               __read_only image2d_t  adjusted,
                                               __write_only image2d_t dst,
                                               __read_only image2d_t mask, float grade_mix) {
  const int2 gid  = (int2)((int)get_global_id(0), (int)get_global_id(1));
  const int2 size = get_image_dim(dst);
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  const float4 a   = read_imagef(adjusted, kNearestClamp, gid);
  const float4 s   = read_imagef(source, kNearestClamp, gid);
  const float  mix = clamp(grade_mix * read_imagef(mask, kNearestClamp, gid).x, 0.0f, 1.0f);
  write_imagef(dst, gid, (float4)(s.xyz + (a.xyz - s.xyz) * mix, s.w));
}
