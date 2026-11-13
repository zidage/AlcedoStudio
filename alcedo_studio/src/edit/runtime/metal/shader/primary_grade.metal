//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <metal_stdlib>

using namespace metal;
#include "oklab_math.h"

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

// Contrast: S curve on OkLab lightness of scene-linear AP1 (oklab_math.h) around 18% grey, chroma
// scaled by sqrt(k) * min(gain, 1). Same math as ApplyOkLabContrast in cuda_primary_grade_pass.cu
// (documented there) and tests/edit/runtime/oklab_contrast_reference.hpp. precise:: math keeps
// fast-math from moving this operator away from the CUDA and OpenCL results.
static inline float3 ApplyOkLabContrast(float3 acescc, float contrast) {
  constexpr float kPivotLightness  = 0.5646216f;  // cbrt(0.18)
  constexpr float kCurveWidthStops = 2.5f;
  const float     slope            = precise::exp2(contrast * 0.01f);
  float3          lab              = LinearAp1ToOkLab(AcesccToLinearAp1(acescc));
  const float shape =
      lab.x > 0.0f ? precise::tanh(3.0f * precise::log2(lab.x / kPivotLightness) / kCurveWidthStops)
                   : -1.0f;
  const float gain         = precise::exp2((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
  const float chroma_scale = precise::sqrt(slope) * min(gain, 1.0f);
  lab                      = float3(lab.x * gain, lab.y * chroma_scale, lab.z * chroma_scale);
  return LinearAp1ToAcescc(OkLabToLinearAp1(lab));
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

// Tone color wheels (edit/operators/models/color_wheel_tone.hpp). values[0..2]: Offset as an ACEScc
// increment; [3..5]: Lift K / R; [6..8]: Gamma exponent; [9..11]: Gain K / R; [12]: ACEScc at
// t = 0; [13]: ACEScc width of the tone range.
static inline float ApplyToneWheel(float c, device const GradeAdjustmentParams& p, int ch) {
  // Neutral Lift, Gamma, and Gain leave the value exact (only the Offset applies).
  if (p.values[3 + ch] == 0.0f && p.values[6 + ch] == 1.0f && p.values[9 + ch] == 0.0f) {
    return c + p.values[ch];
  }
  const float black = p.values[12];
  const float range = p.values[13];
  float       t     = (c + p.values[ch] - black) / range;
  t += p.values[3 + ch] * clamp(1.0f - t, 0.0f, 1.0f);
  if (t > 0.0f && t < 1.0f) t = pow(t, p.values[6 + ch]);
  t += p.values[9 + ch] * max(t, 0.0f);
  return black + t * range;
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
    // values[1..9]: row-major CAT02 adaptation in linear AP1 (resolved on the CPU).
    const float3 lin = float3(CeAcesccDecode(c.x), CeAcesccDecode(c.y),
                              CeAcesccDecode(c.z));
    c = float3(CeAcesccEncode(p.values[1] * lin.x + p.values[2] * lin.y + p.values[3] * lin.z),
               CeAcesccEncode(p.values[4] * lin.x + p.values[5] * lin.y + p.values[6] * lin.z),
               CeAcesccEncode(p.values[7] * lin.x + p.values[8] * lin.y + p.values[9] * lin.z));
  } else if (behavior == 1u) {
    const float offset = value / CE_ACESCC_B;
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
    c.x = ApplyToneWheel(c.x, p, 0);
    c.y = ApplyToneWheel(c.y, p, 1);
    c.z = ApplyToneWheel(c.z, p, 2);
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
  } else if (behavior == 12u && value > 0.0f && lut_edge > 1u) {
    // value is the LUT strength a in (0, 1]: c + a * (L(c) - c) in ACEScc; a = 1 is L(c).
    // An empty LUT (edge 0) means the referenced file is missing: the operation is skipped.
    const float  scale   = float(lut_edge - 1u) / float(lut_edge);
    const float  offset  = 1.0f / (2.0f * float(lut_edge));
    const float3 sampled =
        SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset, c.z * scale + offset);
    c = value >= 1.0f ? sampled : c + value * (sampled - c);
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
