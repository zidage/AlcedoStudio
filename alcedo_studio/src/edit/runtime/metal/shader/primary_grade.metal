//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;

struct GradeAdjustmentParams {
  uint  behavior;
  uint  count;
  float values[48];
  uint  reserved[2];
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

// Contrast: S curve on OkLab lightness of scene-linear AP1 around 18% grey, chroma scaled by
// sqrt(k) * min(gain, 1). Same math as ApplyOkLabContrast in cuda_primary_grade_pass.cu (documented
// there) and tests/edit/runtime/oklab_contrast_reference.hpp. precise:: math keeps fast-math from
// moving this operator away from the CUDA and OpenCL results.
static inline float ContrastAcesccEncode(float value) {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (precise::log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (precise::log2(value) + kA) / kB;
}

static inline float ContrastAcesccDecode(float value) {
  constexpr float kA         = 9.72f;
  constexpr float kB         = 17.52f;
  constexpr float kOffset    = 0.0000152587890625f;
  constexpr float kFloor     = (-16.0f + kA) / kB;
  constexpr float kThreshold = (-15.0f + kA) / kB;
  if (value < kFloor) {
    return value - kFloor;
  }
  if (value <= kThreshold) {
    return (precise::exp2(value * kB - kA) - kOffset) * 2.0f;
  }
  return precise::exp2(value * kB - kA);
}

static inline float SignedCbrt(float value) {
  return copysign(precise::pow(fabs(value), 1.0f / 3.0f), value);
}

static inline float3 LinearAp1ToOkLab(float3 c) {
  const float l  = 0.6341104672f * c.x + 0.3489495087f * c.y + 0.0169400240f * c.z;
  const float m  = 0.2754060131f * c.x + 0.6327713632f * c.y + 0.0918226237f * c.z;
  const float s  = 0.1056775254f * c.x + 0.1971481306f * c.y + 0.6971743440f * c.z;
  const float l_ = SignedCbrt(l);
  const float m_ = SignedCbrt(m);
  const float s_ = SignedCbrt(s);
  return float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static inline float3 OkLabToLinearAp1(float3 lab) {
  const float l_ = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
  const float m_ = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
  const float s_ = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
  const float l  = l_ * l_ * l_;
  const float m  = m_ * m_ * m_;
  const float s  = s_ * s_ * s_;
  return float3(2.0693822907f * l - 1.1736821545f * m + 0.1042998638f * s,
                -0.8917480677f * l + 2.1537429245f * m - 0.2619948569f * s,
                -0.0615064732f * l - 0.4311325686f * m + 1.4926390418f * s);
}

static inline float3 ApplyOkLabContrast(float3 acescc, float contrast) {
  constexpr float kPivotLightness  = 0.5646216f;  // cbrt(0.18)
  constexpr float kCurveWidthStops = 2.5f;
  const float     slope            = precise::exp2(contrast * 0.01f);
  float3          lab =
      LinearAp1ToOkLab(float3(ContrastAcesccDecode(acescc.x), ContrastAcesccDecode(acescc.y),
                              ContrastAcesccDecode(acescc.z)));
  const float shape =
      lab.x > 0.0f ? precise::tanh(3.0f * precise::log2(lab.x / kPivotLightness) / kCurveWidthStops)
                   : -1.0f;
  const float gain         = precise::exp2((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
  const float chroma_scale = precise::sqrt(slope) * min(gain, 1.0f);
  lab                      = float3(lab.x * gain, lab.y * chroma_scale, lab.z * chroma_scale);
  const float3 linear_ap1  = OkLabToLinearAp1(lab);
  return float3(ContrastAcesccEncode(linear_ap1.x), ContrastAcesccEncode(linear_ap1.y),
                ContrastAcesccEncode(linear_ap1.z));
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
  if (chroma <= 1.0e-6f) return c;
  float sum_h = 0.0f, sum_l = 0.0f, sum_s = 0.0f, sum_weight = 0.0f;
  for (int i = 0; i < 8; ++i) {
    const float difference = abs(hue - p.values[i]);
    const float distance   = min(difference, 360.0f - difference);
    const float width      = max(p.values[32 + i], 1.0f);
    const float weight     = exp2(-distance * distance / (width * width));
    sum_h += p.values[8 + i * 3] * weight;
    sum_l += p.values[8 + i * 3 + 1] * weight;
    sum_s += p.values[8 + i * 3 + 2] * weight;
    sum_weight += weight;
  }
  if (sum_weight <= 1.0e-6f) return c;
  const float inv_weight = 1.0f / sum_weight;
  const float adj_h = sum_h * inv_weight;
  const float adj_l = sum_l * inv_weight;
  const float adj_s = sum_s * inv_weight;
  if (abs(adj_h) <= 1.0e-6f && abs(adj_l) <= 1.0e-6f && abs(adj_s) <= 1.0e-6f) return c;
  const float angle = adj_h * 2.25f * 0.017453292519943295f;
  const float scale = exp2(adj_s * 2.25f * (adj_s >= 0.0f ? 4.5f : 3.25f));
  const float luma  = Luma(c) + adj_l * 1.125f;
  const float i     = 0.596f * c.x - 0.274f * c.y - 0.322f * c.z;
  const float q     = 0.211f * c.x - 0.523f * c.y + 0.312f * c.z;
  const float ri    = (i * cos(angle) - q * sin(angle)) * scale;
  const float rq    = (i * sin(angle) + q * cos(angle)) * scale;
  return float3(luma + 0.956f * ri + 0.621f * rq, luma - 0.272f * ri - 0.647f * rq,
                luma - 1.106f * ri + 1.703f * rq);
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
  } else if (behavior == 2u && value != 0.0f) {
    c = ApplyOkLabContrast(c, value);
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
    float scale = behavior == 9u ? value : 1.0f + value * 0.01f;
    if (behavior == 10u) {
      const float maximum = max(c.x, max(c.y, c.z));
      const float minimum = min(c.x, min(c.y, c.z));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - min(maximum - minimum, 1.0f));
    }
    const float l = Luma(c);
    if (scale > 1.5f) {
      const float peak       = max(c.x, max(c.y, c.z));
      const float peak_raise = (peak - l) * (scale - 1.0f);
      if (peak_raise > 0.1f) scale = 1.0f + 0.1f / max(peak - l, 1.0e-6f);
    }
    c             = l + (c - l) * scale;
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
    c = l + (c - l) * scale;
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
                                    texture2d<float, access::read> mix_source [[texture(2)]],
                                    texture2d<float, access::read> mask [[texture(3)]],
                                    device const uchar* parameter_base [[buffer(0)]],
                                    device const uint* commands [[buffer(1)]],
                                    constant PrimaryGradeDispatchParams& dispatch [[buffer(2)]],
                                    device const float4* lmt_lut [[buffer(3)]],
                                    constant float& grade_mix [[buffer(4)]],
                                    constant uint& apply_mix [[buffer(5)]],
                                    constant uint& has_mask [[buffer(6)]],
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
  float4 adjusted = float4(c, source.w);
  if (apply_mix != 0u) {
    const float4 original = mix_source.read(gid);
    float mix = grade_mix;
    if (has_mask != 0u) {
      mix *= mask.read(gid).r;
    }
    mix      = clamp(mix, 0.0f, 1.0f);
    adjusted = float4(original.xyz + (adjusted.xyz - original.xyz) * mix, original.w);
  }
  dst.write(adjusted, gid);
}

kernel void primary_grade_mix(texture2d<float, access::read> source [[texture(0)]],
                              texture2d<float, access::read> adjusted [[texture(1)]],
                              texture2d<float, access::write> dst [[texture(2)]],
                              constant float& grade_mix [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= source.get_width() || gid.y >= source.get_height()) {
    return;
  }
  const float4 a   = adjusted.read(gid);
  const float4 s   = source.read(gid);
  const float  mix = clamp(grade_mix, 0.0f, 1.0f);
  dst.write(float4(s.xyz + (a.xyz - s.xyz) * mix, s.w), gid);
}

kernel void primary_grade_mix_masked(texture2d<float, access::read> source [[texture(0)]],
                                     texture2d<float, access::read> adjusted [[texture(1)]],
                                     texture2d<float, access::write> dst [[texture(2)]],
                                     texture2d<float, access::read> mask [[texture(3)]],
                                     constant float& grade_mix [[buffer(0)]],
                                     uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= source.get_width() || gid.y >= source.get_height()) {
    return;
  }
  const float4 a   = adjusted.read(gid);
  const float4 s   = source.read(gid);
  const float  mix = clamp(grade_mix * mask.read(gid).r, 0.0f, 1.0f);
  dst.write(float4(s.xyz + (a.xyz - s.xyz) * mix, s.w), gid);
}
