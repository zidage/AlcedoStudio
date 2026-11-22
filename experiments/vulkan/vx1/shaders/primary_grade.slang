//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX1 experiment (vulkan_compute_backend_plan.md, section 6): the point kernel of the Color Grade,
// ported from edit/runtime/opencl/shader/primary_grade.cl and oklab.cl. Experiment code: it does
// not merge.
//
// Binding model of the experiment:
// - set 0, binding 0: source RGBA32F storage image (read)
// - set 0, binding 1: destination RGBA32F storage image (write)
// - the parameter arena, the command list, and the LUT are device addresses in the push constants.

#include "color/color_encoding_math.h"

// --- oklab.cl -----------------------------------------------------------------------------------

// OpenCL C cbrt has 2 ulp. SPIR-V (GLSL.std.450) has no cbrt: exp2(log2 |x| / 3) and one Newton
// step. 0 stays 0.
// VX1_CBRT_NEWTON (build option): Newton steps after exp2(log2 |x| / 3). Default 1.
#ifndef VX1_CBRT_NEWTON
#define VX1_CBRT_NEWTON 1
#endif
static float Cbrt(float x) {
  if (x == 0.0f) return 0.0f;
  const float a = abs(x);
  float       r = exp2(log2(a) * (1.0f / 3.0f));
  for (int i = 0; i < VX1_CBRT_NEWTON; ++i) {
    r = r - (r * r * r - a) / (3.0f * r * r);
  }
  return x < 0.0f ? -r : r;
}

static float3 AcesccToLinearAp1(float3 acescc) {
  return float3(CeAcesccDecode(acescc.x), CeAcesccDecode(acescc.y), CeAcesccDecode(acescc.z));
}

static float3 LinearAp1ToAcescc(float3 linear_ap1) {
  return float3(CeAcesccEncode(linear_ap1.x), CeAcesccEncode(linear_ap1.y),
                CeAcesccEncode(linear_ap1.z));
}

static float3 LinearAp1ToOkLab(float3 c) {
  const float l  = 0.6341104672f * c.x + 0.3489495087f * c.y + 0.0169400240f * c.z;
  const float m  = 0.2754060131f * c.x + 0.6327713632f * c.y + 0.0918226237f * c.z;
  const float s  = 0.1056775254f * c.x + 0.1971481306f * c.y + 0.6971743440f * c.z;
  const float l_ = Cbrt(l);
  const float m_ = Cbrt(m);
  const float s_ = Cbrt(s);
  return float3(0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_);
}

static float3 OkLabToLinearAp1(float3 lab) {
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

// --- primary_grade.cl ---------------------------------------------------------------------------

struct GradeAdjustmentParams {
  uint  behavior;
  uint  count;
  float values[48];
  uint  reserved[2];
};

struct PrimaryGradePush {
  uint64_t parameter_base;  // the parameter arena (bytes)
  uint*    commands;        // record offsets in bytes
  float4*  lut;
  uint     command_count;
  uint     command_offset;
  uint     lut_edge;
  uint     width;
  uint     height;
};

[[vk::push_constant]] PrimaryGradePush push;

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> src_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> dst_image;

static float Luma(float3 c) { return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z; }

static float3 ApplyOkLabContrast(float3 acescc, float contrast) {
  const float kPivotLightness  = 0.5646216f;  // cbrt(0.18)
  const float kCurveWidthStops = 2.5f;
  const float slope            = exp2(contrast * 0.01f);
  float3      lab              = LinearAp1ToOkLab(AcesccToLinearAp1(acescc));
  const float shape =
      lab.x > 0.0f ? tanh(3.0f * log2(lab.x / kPivotLightness) / kCurveWidthStops) : -1.0f;
  const float gain         = exp2((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
  const float chroma_scale = sqrt(slope) * min(gain, 1.0f);
  lab                      = float3(lab.x * gain, lab.y * chroma_scale, lab.z * chroma_scale);
  return LinearAp1ToAcescc(OkLabToLinearAp1(lab));
}

static float ToneZoneShiftEv(float x, float s) {
  if (x <= 0.0f || s == 0.0f) return 0.0f;
  const float w = 3.0f;
  if (s > 0.0f) {
    const float t = min(x / w, 1.0f);
    return s * t * t * (3.0f - 2.0f * t);
  }
  const float a = -s;
  if (x >= w + a) return -a;
  float t = x / (w + a);
  for (int i = 0; i < 5; ++i) {
    const float f  = w * t + a * t * t * (3.0f - 2.0f * t) - x;
    const float fp = w + 6.0f * a * t * (1.0f - t);
    t              = clamp(t - f / fp, 0.0f, 1.0f);
  }
  return w * t - x;
}

// VX1_CURVE_FORM (build option), for the Intel driver failure of VX1:
// 0: element pointers into the record, as primary_grade.cl (float* xs = &p->values[0])
// 1: the record copied into a local value, then array indexing
// 2: direct indexing through the record pointer (p->values[16 + i])
#ifndef VX1_CURVE_FORM
#define VX1_CURVE_FORM 0
#endif
// VX1_CURVE_SEARCH: 0 the while loop of primary_grade.cl, 1 a loop of fixed count.
#ifndef VX1_CURVE_SEARCH
#define VX1_CURVE_SEARCH 0
#endif
// VX1_RECORD_FORM: 0 the records of the Curve slot as p + k (primary_grade.cl), 1 as an address
// computed in uint64. Default 1: p + k on a struct pointer makes vkCreateComputePipelines of the
// Intel driver fail when the function is inlined (VX1 record, 2026-11-22).
#ifndef VX1_RECORD_FORM
#define VX1_RECORD_FORM 1
#endif
#if VX1_RECORD_FORM == 0
#define VX1_RECORD(p, k) ((p) + (k))
#else
#define VX1_RECORD(p, k) \
  ((GradeAdjustmentParams*)((uint64_t)(p) + uint64_t(k) * uint64_t(sizeof(GradeAdjustmentParams))))
#endif
static float ApplyCurve(float value, GradeAdjustmentParams* p) {
  const uint n = p->count;
  if (n < 2u) {
    return value;
  }
#if VX1_CURVE_FORM == 0
  float* xs = &p->values[0];
  float* ys = &p->values[16];
  float* ms = &p->values[32];
#define XS(i) xs[(i)]
#define YS(i) ys[(i)]
#define MS(i) ms[(i)]
#elif VX1_CURVE_FORM == 1
  const GradeAdjustmentParams r = *p;
#define XS(i) r.values[(i)]
#define YS(i) r.values[16u + (i)]
#define MS(i) r.values[32u + (i)]
#else
#define XS(i) p->values[(i)]
#define YS(i) p->values[16u + (i)]
#define MS(i) p->values[32u + (i)]
#endif
  if (value <= XS(0u)) {
    return YS(0u) + MS(0u) * (value - XS(0u));
  }
  if (value >= XS(n - 1u)) {
    return YS(n - 1u) + MS(n - 1u) * (value - XS(n - 1u));
  }
  uint i = 0u;
#if VX1_CURVE_SEARCH == 0
  while (i + 2u < n && value >= XS(i + 1u)) {
    ++i;
  }
#else
  // The same cell with a loop of fixed count: the x values increase, so the last k that passes
  // is the cell of the while loop.
  for (uint k = 1u; k < 15u; ++k) {
    if (k + 1u < n && value >= XS(k)) i = k;
  }
#endif
  const float h  = XS(i + 1u) - XS(i);
  const float t  = (value - XS(i)) / h;
  const float dy = YS(i + 1u) - YS(i);
  const float d  = dy / h;
  const float u  = 1.0f - t;
  return YS(i) + t * dy + h * t * u * (u * (MS(i) - d) - t * (MS(i + 1u) - d));
#undef XS
#undef YS
#undef MS
}

static float ApplyToneWheel(float c, GradeAdjustmentParams* p, int ch) {
  if (p->values[3 + ch] == 0.0f && p->values[6 + ch] == 1.0f && p->values[9 + ch] == 0.0f) {
    return c + p->values[ch];
  }
  const float black = p->values[12];
  const float range = p->values[13];
  float       t     = (c + p->values[ch] - black) / range;
  t += p->values[3 + ch] * clamp(1.0f - t, 0.0f, 1.0f);
  if (t > 0.0f && t < 1.0f) t = pow(t, p->values[6 + ch]);
  t += p->values[9 + ch] * max(t, 0.0f);
  return black + t * range;
}

static float3 ApplyHls(float3 c, GradeAdjustmentParams* p) {
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
    const float difference = abs(hue - p->values[i]);
    const float distance   = min(difference, 360.0f - difference);
    const float width      = max(p->values[32 + i], 1.0f);
    const float weight     = exp2(-distance * distance / (width * width));
    sum_h += p->values[8 + i * 3] * weight;
    sum_l += p->values[8 + i * 3 + 1] * weight;
    sum_s += p->values[8 + i * 3 + 2] * weight;
    sum_weight += weight;
  }
  if (sum_weight <= 1.0e-6f) return c;
  const float inv_weight = 1.0f / sum_weight;
  const float adj_h      = sum_h * inv_weight;
  const float adj_l      = sum_l * inv_weight;
  const float adj_s      = sum_s * inv_weight;
  if (abs(adj_h) <= 1.0e-6f && abs(adj_l) <= 1.0e-6f && abs(adj_s) <= 1.0e-6f) return c;
  const float angle = -adj_h * 2.25f * 0.017453292519943295f;
  const float scale = exp2(adj_s * 2.25f * (adj_s >= 0.0f ? 4.5f : 3.25f));
  const float lift  = adj_l * 1.125f;
  const float i     = 0.596f * c.x - 0.274f * c.y - 0.322f * c.z;
  const float q     = 0.211f * c.x - 0.523f * c.y + 0.312f * c.z;
  const float di    = (i * cos(angle) - q * sin(angle)) * scale - i;
  const float dq    = (i * sin(angle) + q * cos(angle)) * scale - q;
  return float3(c.x + lift + 0.956f * di + 0.621f * dq, c.y + lift - 0.272f * di - 0.647f * dq,
                c.z + lift - 1.106f * di + 1.703f * dq);
}

// VX1_BEHAVIOR_MASK (build option): the behaviors that ApplyAdjustment has, one bit each. The
// default has all of them. A smaller mask is for the bisection of a driver failure.
#ifndef VX1_BEHAVIOR_MASK
#define VX1_BEHAVIOR_MASK 0xFFFFFFFFu
#endif
#define VX1_ON(b) ((((VX1_BEHAVIOR_MASK) >> (b)) & 1u) != 0u)

static uint LutIndex(uint edge, uint x, uint y, uint z) { return (z * edge + y) * edge + x; }

static float3 SampleLut3d(float4* lut, uint edge, float u, float v, float w) {
  const float3 coord   = clamp(float3(u, v, w), 0.0f, 1.0f);
  const float3 tex_pos = coord * float(edge) - 0.5f;
  const float3 pos     = clamp(tex_pos, 0.0f, float(edge - 1u));
  const uint3  lo      = uint3(pos);
  const uint3  hi      = min(lo + uint3(1u), uint3(edge - 1u));
  const float3 t       = pos - float3(lo);
  const float4 c000    = lut[LutIndex(edge, lo.x, lo.y, lo.z)];
  const float4 c100    = lut[LutIndex(edge, hi.x, lo.y, lo.z)];
  const float4 c010    = lut[LutIndex(edge, lo.x, hi.y, lo.z)];
  const float4 c110    = lut[LutIndex(edge, hi.x, hi.y, lo.z)];
  const float4 c001    = lut[LutIndex(edge, lo.x, lo.y, hi.z)];
  const float4 c101    = lut[LutIndex(edge, hi.x, lo.y, hi.z)];
  const float4 c011    = lut[LutIndex(edge, lo.x, hi.y, hi.z)];
  const float4 c111    = lut[LutIndex(edge, hi.x, hi.y, hi.z)];
  const float4 c00     = lerp(c000, c100, t.x);
  const float4 c10     = lerp(c010, c110, t.x);
  const float4 c01     = lerp(c001, c101, t.x);
  const float4 c11     = lerp(c011, c111, t.x);
  const float4 c0      = lerp(c00, c10, t.y);
  const float4 c1      = lerp(c01, c11, t.y);
  const float4 sampled = lerp(c0, c1, t.z);
  return sampled.xyz;
}

static float3 ApplyAdjustment(float3 c, GradeAdjustmentParams* p, float4* lut, uint lut_edge) {
  const uint  behavior = p->behavior;
  const float value    = p->values[0];
  if (VX1_ON(0) && behavior == 0u && value != 0.0f) {
    // values[1..9]: row-major CAT02 adaptation in linear AP1.
#if VX1_CURVE_FORM == 2
    // No address-of: the Metal target of Slang has no '&' (VX1 record).
#define M(i) p->values[1 + (i)]
#else
    float* m = &p->values[1];
#define M(i) m[(i)]
#endif
    const float3 lin = float3(CeAcesccDecode(c.x), CeAcesccDecode(c.y), CeAcesccDecode(c.z));
    c = float3(CeAcesccEncode(M(0) * lin.x + M(1) * lin.y + M(2) * lin.z),
               CeAcesccEncode(M(3) * lin.x + M(4) * lin.y + M(5) * lin.z),
               CeAcesccEncode(M(6) * lin.x + M(7) * lin.y + M(8) * lin.z));
#undef M
  } else if (VX1_ON(1) && behavior == 1u) {
    const float offset = value / CE_ACESCC_B;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (VX1_ON(2) && behavior == 2u && value != 0.0f) {
    c = ApplyOkLabContrast(c, value);
  } else if (VX1_ON(3) && (behavior == 3u || behavior == 4u) && value != 0.0f) {
    const float x        = (Luma(c) - CE_ACESCC_MIDDLE_GREY) * CE_ACESCC_B;
    const float s        = value * 0.03f;
    const float shift_ev = behavior == 3u ? ToneZoneShiftEv(x, s) : -ToneZoneShiftEv(-x, -s);
    const float offset   = shift_ev / CE_ACESCC_B;
    c.x += offset;
    c.y += offset;
    c.z += offset;
  } else if (VX1_ON(7) && behavior == 7u) {
    c.x = ApplyCurve(ApplyCurve(c.x, p), VX1_RECORD(p, 1));
    c.y = ApplyCurve(ApplyCurve(c.y, p), VX1_RECORD(p, 2));
    c.z = ApplyCurve(ApplyCurve(c.z, p), VX1_RECORD(p, 3));
  } else if (VX1_ON(8) && behavior == 8u) {
    c = ApplyHls(c, p);
  } else if (VX1_ON(9) && (behavior == 9u || behavior == 10u)) {
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
    c = l + (c - l) * scale;
  } else if (VX1_ON(11) && behavior == 11u) {
    c.x = ApplyToneWheel(c.x, p, 0);
    c.y = ApplyToneWheel(c.y, p, 1);
    c.z = ApplyToneWheel(c.z, p, 2);
  } else if (VX1_ON(12) && behavior == 12u && value > 0.0f && lut_edge > 1u) {
    const float  scale   = float(lut_edge - 1u) / float(lut_edge);
    const float  offset  = 1.0f / (2.0f * float(lut_edge));
    const float3 sampled = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset,
                                       c.z * scale + offset);
    c = value >= 1.0f ? sampled : c + value * (sampled - c);
  }
  return c;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void primary_grade_pointwise_rgba32f(uint3 id: SV_DispatchThreadID) {
  if (id.x >= push.width || id.y >= push.height) return;
  const float4 source = src_image[id.xy];
  float3       c      = source.xyz;
  for (uint i = 0u; i < push.command_count; ++i) {
    const uint             offset = push.commands[push.command_offset + i];
    GradeAdjustmentParams* params = (GradeAdjustmentParams*)(push.parameter_base + offset);
    c = ApplyAdjustment(c, params, push.lut, push.lut_edge);
  }
  dst_image[id.xy] = float4(c.x, c.y, c.z, source.w);
}
