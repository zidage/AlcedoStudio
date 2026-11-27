//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group primary_grade (plan #1036, phase VK8): the point kernels of the Color Grade of
// opencl/shader/primary_grade.cl that the Grade pass calls, primary_grade_pointwise_scene_rgba32f
// and primary_grade_curve_input_histogram. The names, the bindings, and the push constant layouts
// are in include/vulkan/vulkan_primary_grade_kernels.hpp.
//
// A scene image is a cached RGBA32F storage image or a scene-work member (an RGBA32F row-major
// buffer). A kernel reads the buffer when its address is not 0, else the image of its binding.
// The adjustment records (GradeAdjustmentParams, 208 bytes) are read by address arithmetic on the
// parameter arena: a pointer to a record struct crashes the Intel compiler (phase VK4 record).

#include "color/color_encoding_math.h"
#include "oklab.slangh"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> mix_image;
[[vk::binding(2, 0)]] [vk::image_format("r8")] RWTexture2D<float> mask_image;

// PrimaryGradePointwisePushConstants: 80 bytes.
struct PrimaryGradePointwisePushConstants {
  uint64_t parameter_base;  // the parameter arena buffer
  uint64_t commands;        // uint offsets of the records, in apply order
  uint64_t lut;             // float4 voxels, X fastest; a 16-byte dummy when lut_edge < 2
  uint64_t source;          // float4 row-major, or 0: source_image
  uint64_t destination;     // float4 row-major (a scene-work member)
  uint64_t mix_source;      // float4 row-major, or 0: mix_image
  uint     command_offset;
  uint     command_count;
  uint     lut_edge;
  uint     width;
  uint     height;
  uint     apply_mix;  // 0: no mix; 1: mix with the mix source
  uint     has_mask;   // 1: the mix is multiplied by mask_image
  float    grade_mix;
};

// CurveInputHistogramPushConstants: 56 bytes.
struct CurveInputHistogramPushConstants {
  uint64_t parameter_base;
  uint64_t commands;  // uint offsets of the records before the Curve
  uint64_t source;    // float4 row-major, or 0: source_image
  uint64_t counts;    // 768 uint: red bins 0..255, green 256..511, blue 512..767
  uint     width;
  uint     step;
  uint     sampled_width;
  uint     sample_count;
  uint     command_count;
  uint     invocation_count;  // workgroups * 256: the stride of the sample loop
};

// Byte layout of GradeAdjustmentParams (adjustment_runtime.hpp).
static const uint kRecordBytes       = 208u;
static const uint kRecordValuesBytes = 8u;

static float4* Float4At(uint64_t address) { return (float4*)address; }

// values[index] of the record at @p record.
static float RecordValue(uint64_t record, uint index) {
  return *((float*)(record + kRecordValuesBytes + uint64_t(index) * 4u));
}

static uint RecordBehavior(uint64_t record) { return *((uint*)record); }
static uint RecordCount(uint64_t record) { return *((uint*)(record + 4u)); }

static float Luma(float3 c) { return 0.272229f * c.x + 0.674082f * c.y + 0.053689f * c.z; }

// tanh with the error of AlcedoExp: (1 - e^-2|x|) / (1 + e^-2|x|) with the sign of x.
static float AlcedoTanhLocal(float x) {
  const float t = AlcedoExp(-2.0f * abs(x));
  const float r = (1.0f - t) / (1.0f + t);
  return x < 0.0f ? -r : r;
}

// Contrast: the S curve on OkLab lightness of primary_grade.cl (ApplyOkLabContrast).
static float3 ApplyOkLabContrast(float3 acescc, float contrast) {
  const float kPivotLightness  = 0.5646216f;  // cbrt(0.18)
  const float kCurveWidthStops = 2.5f;
  const float slope            = AlcedoExp2(contrast * 0.01f);
  float3      lab              = LinearAp1ToOkLab(AcesccToLinearAp1(acescc));
  const float shape            = lab.x > 0.0f
                                     ? AlcedoTanhLocal(3.0f * AlcedoLog2(lab.x / kPivotLightness) /
                                                       kCurveWidthStops)
                                     : -1.0f;
  const float gain             = AlcedoExp2((slope - 1.0f) * kCurveWidthStops * shape / 3.0f);
  const float chroma_scale     = sqrt(slope) * min(gain, 1.0f);
  lab = float3(lab.x * gain, lab.y * chroma_scale, lab.z * chroma_scale);
  return LinearAp1ToAcescc(OkLabToLinearAp1(lab));
}

// Whites and Blacks: ToneZoneShiftEv of primary_grade.cl.
static float ToneZoneShiftEv(float x, float s) {
  if (x <= 0.0f || s == 0.0f) {
    return 0.0f;
  }
  const float w = 3.0f;
  if (s > 0.0f) {
    const float t = min(x / w, 1.0f);
    return s * t * t * (3.0f - 2.0f * t);
  }
  const float a = -s;
  if (x >= w + a) {
    return -a;
  }
  float t = x / (w + a);
  for (int i = 0; i < 5; ++i) {
    const float f  = w * t + a * t * t * (3.0f - 2.0f * t) - x;
    const float fp = w + 6.0f * a * t * (1.0f - t);
    t              = clamp(t - f / fp, 0.0f, 1.0f);
  }
  return w * t - x;
}

// Tone Curve spline of primary_grade.cl (ApplyCurve): values[0..15] x, [16..31] y, [32..47] slope.
static float ApplyCurve(float value, uint64_t record) {
  const uint n = RecordCount(record);
  if (n < 2u) {
    return value;
  }
  const float x0 = RecordValue(record, 0u);
  if (value <= x0) {
    return RecordValue(record, 16u) + RecordValue(record, 32u) * (value - x0);
  }
  const float xn = RecordValue(record, n - 1u);
  if (value >= xn) {
    return RecordValue(record, 16u + n - 1u) + RecordValue(record, 32u + n - 1u) * (value - xn);
  }
  uint i = 0u;
  while (i + 2u < n && value >= RecordValue(record, i + 1u)) {
    ++i;
  }
  const float xi  = RecordValue(record, i);
  const float h   = RecordValue(record, i + 1u) - xi;
  const float t   = (value - xi) / h;
  const float yi  = RecordValue(record, 16u + i);
  const float dy  = RecordValue(record, 16u + i + 1u) - yi;
  const float d   = dy / h;
  const float u   = 1.0f - t;
  const float mi  = RecordValue(record, 32u + i);
  const float mi1 = RecordValue(record, 32u + i + 1u);
  return yi + t * dy + h * t * u * (u * (mi - d) - t * (mi1 - d));
}

// Tone color wheels of primary_grade.cl (ApplyToneWheel).
static float ApplyToneWheel(float c, uint64_t record, uint ch) {
  const float offset = RecordValue(record, ch);
  const float lift   = RecordValue(record, 3u + ch);
  const float gamma  = RecordValue(record, 6u + ch);
  const float gain   = RecordValue(record, 9u + ch);
  // Neutral Lift, Gamma, and Gain leave the value exact (only the Offset applies).
  if (lift == 0.0f && gamma == 1.0f && gain == 0.0f) {
    return c + offset;
  }
  const float black = RecordValue(record, 12u);
  const float range = RecordValue(record, 13u);
  float       t     = (c + offset - black) / range;
  t += lift * clamp(1.0f - t, 0.0f, 1.0f);
  if (t > 0.0f && t < 1.0f) {
    t = AlcedoPow(t, gamma);
  }
  t += gain * max(t, 0.0f);
  return black + t * range;
}

// HLS of primary_grade.cl (ApplyHls).
static float3 ApplyHls(float3 c, uint64_t record) {
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
  if (chroma <= 1.0e-6f) {
    return c;
  }
  float sum_h = 0.0f, sum_l = 0.0f, sum_s = 0.0f, sum_weight = 0.0f;
  for (uint i = 0u; i < 8u; ++i) {
    const float difference = abs(hue - RecordValue(record, i));
    const float distance   = min(difference, 360.0f - difference);
    const float width      = max(RecordValue(record, 32u + i), 1.0f);
    const float weight     = AlcedoExp2(-distance * distance / (width * width));
    sum_h += RecordValue(record, 8u + i * 3u) * weight;
    sum_l += RecordValue(record, 8u + i * 3u + 1u) * weight;
    sum_s += RecordValue(record, 8u + i * 3u + 2u) * weight;
    sum_weight += weight;
  }
  if (sum_weight <= 1.0e-6f) {
    return c;
  }
  const float inv_weight = 1.0f / sum_weight;
  const float adj_h      = sum_h * inv_weight;
  const float adj_l      = sum_l * inv_weight;
  const float adj_s      = sum_s * inv_weight;
  if (abs(adj_h) <= 1.0e-6f && abs(adj_l) <= 1.0e-6f && abs(adj_s) <= 1.0e-6f) {
    return c;
  }
  // Hue Shift and Chroma rotate and scale the YIQ chroma (I, Q); Lightness adds to every channel.
  const float angle = -adj_h * 2.25f * 0.017453292519943295f;
  const float scale = AlcedoExp2(adj_s * 2.25f * (adj_s >= 0.0f ? 4.5f : 3.25f));
  const float lift  = adj_l * 1.125f;
  const float i     = 0.596f * c.x - 0.274f * c.y - 0.322f * c.z;
  const float q     = 0.211f * c.x - 0.523f * c.y + 0.312f * c.z;
  const float cos_a = AlcedoCos(angle);
  const float sin_a = AlcedoSin(angle);
  const float di    = (i * cos_a - q * sin_a) * scale - i;
  const float dq    = (i * sin_a + q * cos_a) * scale - q;
  return float3(c.x + lift + 0.956f * di + 0.621f * dq, c.y + lift - 0.272f * di - 0.647f * dq,
                c.z + lift - 1.106f * di + 1.703f * dq);
}

// a + (b - a) t: the OpenCL mix (lerp can round as a (1 - t) + b t).
static float4 MixLinear(float4 a, float4 b, float t) { return a + (b - a) * t; }

static uint LutIndex(uint edge, uint x, uint y, uint z) { return (z * edge + y) * edge + x; }

// Trilinear sample of the voxel table: SampleLut3d of primary_grade.cl.
static float3 SampleLut3d(uint64_t lut, uint edge, float u, float v, float w) {
  float4*      voxels  = Float4At(lut);
  const float3 coord   = clamp(float3(u, v, w), 0.0f, 1.0f);
  const float3 tex_pos = coord * float(edge) - 0.5f;
  const float3 pos     = clamp(tex_pos, 0.0f, float(edge - 1u));
  const uint3  lo      = uint3(pos);
  const uint3  hi      = min(lo + uint3(1u), uint3(edge - 1u));
  const float3 t       = pos - float3(lo);
  const float4 c000    = voxels[LutIndex(edge, lo.x, lo.y, lo.z)];
  const float4 c100    = voxels[LutIndex(edge, hi.x, lo.y, lo.z)];
  const float4 c010    = voxels[LutIndex(edge, lo.x, hi.y, lo.z)];
  const float4 c110    = voxels[LutIndex(edge, hi.x, hi.y, lo.z)];
  const float4 c001    = voxels[LutIndex(edge, lo.x, lo.y, hi.z)];
  const float4 c101    = voxels[LutIndex(edge, hi.x, lo.y, hi.z)];
  const float4 c011    = voxels[LutIndex(edge, lo.x, hi.y, hi.z)];
  const float4 c111    = voxels[LutIndex(edge, hi.x, hi.y, hi.z)];
  const float4 c00     = MixLinear(c000, c100, t.x);
  const float4 c10     = MixLinear(c010, c110, t.x);
  const float4 c01     = MixLinear(c001, c101, t.x);
  const float4 c11     = MixLinear(c011, c111, t.x);
  const float4 c0      = MixLinear(c00, c10, t.y);
  const float4 c1      = MixLinear(c01, c11, t.y);
  return MixLinear(c0, c1, t.z).xyz;
}

// One adjustment record: ApplyAdjustment of primary_grade.cl. The behaviors are the values of
// AdjustmentBehavior; a behavior of a later phase (local tone, neighborhood) is not a point
// operation and leaves the color.
static float3 ApplyAdjustment(float3 c, uint64_t record, uint64_t lut, uint lut_edge) {
  const uint  behavior = RecordBehavior(record);
  const float value    = RecordValue(record, 0u);
  if (behavior == 0u && value != 0.0f) {
    // values[1..9]: row-major CAT02 adaptation in linear AP1 (resolved on the CPU).
    const float3 lin = float3(CeAcesccDecode(c.x), CeAcesccDecode(c.y), CeAcesccDecode(c.z));
    const float  m0  = RecordValue(record, 1u);
    const float  m1  = RecordValue(record, 2u);
    const float  m2  = RecordValue(record, 3u);
    const float  m3  = RecordValue(record, 4u);
    const float  m4  = RecordValue(record, 5u);
    const float  m5  = RecordValue(record, 6u);
    const float  m6  = RecordValue(record, 7u);
    const float  m7  = RecordValue(record, 8u);
    const float  m8  = RecordValue(record, 9u);
    c = float3(CeAcesccEncode(m0 * lin.x + m1 * lin.y + m2 * lin.z),
               CeAcesccEncode(m3 * lin.x + m4 * lin.y + m5 * lin.z),
               CeAcesccEncode(m6 * lin.x + m7 * lin.y + m8 * lin.z));
  } else if (behavior == 1u) {
    const float offset = value / CE_ACESCC_B;
    c += offset;
  } else if (behavior == 2u && value != 0.0f) {
    c = ApplyOkLabContrast(c, value);
  } else if ((behavior == 3u || behavior == 4u) && value != 0.0f) {
    // 3: Whites, 4: Blacks (Whites mirrored about grey).
    const float x        = (Luma(c) - CE_ACESCC_MIDDLE_GREY) * CE_ACESCC_B;
    const float s        = value * 0.03f;
    const float shift_ev = behavior == 3u ? ToneZoneShiftEv(x, s) : -ToneZoneShiftEv(-x, -s);
    c += shift_ev / CE_ACESCC_B;
  } else if (behavior == 7u) {
    // The Curve slot has four records: the RGB curve, then the red, green, and blue curves.
    c.x = ApplyCurve(ApplyCurve(c.x, record), record + kRecordBytes);
    c.y = ApplyCurve(ApplyCurve(c.y, record), record + 2u * kRecordBytes);
    c.z = ApplyCurve(ApplyCurve(c.z, record), record + 3u * kRecordBytes);
  } else if (behavior == 8u) {
    c = ApplyHls(c, record);
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
      if (peak_raise > 0.1f) {
        scale = 1.0f + 0.1f / max(peak - l, 1.0e-6f);
      }
    }
    c = l + (c - l) * scale;
  } else if (behavior == 11u) {
    c.x = ApplyToneWheel(c.x, record, 0u);
    c.y = ApplyToneWheel(c.y, record, 1u);
    c.z = ApplyToneWheel(c.z, record, 2u);
  } else if (behavior == 12u && value > 0.0f && lut_edge > 1u) {
    // value is the LUT strength a in (0, 1]: c + a * (L(c) - c) in ACEScc; a = 1 is L(c).
    const float  scale   = float(lut_edge - 1u) / float(lut_edge);
    const float  offset  = 1.0f / (2.0f * float(lut_edge));
    const float3 sampled = SampleLut3d(lut, lut_edge, c.x * scale + offset, c.y * scale + offset,
                                       c.z * scale + offset);
    c                    = value >= 1.0f ? sampled : c + value * (sampled - c);
  }
  return c;
}

// The records at commands[first .. first + count) applied in order.
static float3 ApplyCommands(float3 c, uint64_t parameter_base, uint64_t commands, uint first,
                            uint count, uint64_t lut, uint lut_edge) {
  for (uint i = 0u; i < count; ++i) {
    const uint offset = *((uint*)(commands + uint64_t(first + i) * 4u));
    c                 = ApplyAdjustment(c, parameter_base + offset, lut, lut_edge);
  }
  return c;
}

// A texel of a scene image: the buffer at @p buffer, or @p image when the address is 0.
static float4 SceneRead(uint64_t buffer, RWTexture2D<float4> image, uint2 gid, uint width) {
  if (buffer != 0u) {
    return Float4At(buffer)[gid.y * width + gid.x];
  }
  return image[gid];
}

// The adjustments of one Grade stage, then the mix with the Grade input (the masked mix of the last
// write): primary_grade_pointwise_scene_rgba32f of primary_grade.cl.
[shader("compute")]
[numthreads(16, 16, 1)]
void primary_grade_pointwise_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                                           uniform PrimaryGradePointwisePushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const uint2  gid    = id.xy;
  const float4 source = SceneRead(push.source, source_image, gid, push.width);
  const float3 c      = ApplyCommands(source.xyz, push.parameter_base, push.commands,
                                      push.command_offset, push.command_count, push.lut,
                                      push.lut_edge);
  float4       adjusted = float4(c, source.w);
  if (push.apply_mix != 0u) {
    const float4 original = SceneRead(push.mix_source, mix_image, gid, push.width);
    float        mix      = push.grade_mix;
    if (push.has_mask != 0u) {
      mix *= mask_image[gid];
    }
    mix      = clamp(mix, 0.0f, 1.0f);
    adjusted = float4(original.xyz + (adjusted.xyz - original.xyz) * mix, original.w);
  }
  Float4At(push.destination)[gid.y * push.width + gid.x] = adjusted;
}

// Bin of one ACEScc value on the curve axis: CurveInputHistogramBin (curve_input_histogram.hpp).
static uint CurveInputHistogramBin(float value) {
  return uint(min(max(value * 256.0f, 0.0f), 255.0f));
}

groupshared uint group_counts[768];

// Histogram of the curve input (issue #888): primary_grade_curve_input_histogram of
// primary_grade.cl. Each invocation takes samples of the grid (column * step, row * step), applies
// the adjustments before the Curve, and adds the three bins to the workgroup counts; each
// workgroup then adds its counts to the counts buffer.
[shader("compute")]
[numthreads(256, 1, 1)]
void primary_grade_curve_input_histogram(uint3 id: SV_DispatchThreadID,
                                         uint3 local_id: SV_GroupThreadID,
                                         uniform CurveInputHistogramPushConstants push) {
  for (uint i = local_id.x; i < 768u; i += 256u) {
    group_counts[i] = 0u;
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint sample = id.x; sample < push.sample_count; sample += push.invocation_count) {
    const uint2  gid    = uint2((sample % push.sampled_width) * push.step,
                                (sample / push.sampled_width) * push.step);
    const float4 source = SceneRead(push.source, source_image, gid, push.width);
    const float3 c      = ApplyCommands(source.xyz, push.parameter_base, push.commands, 0u,
                                        push.command_count, 0u, 0u);
    InterlockedAdd(group_counts[CurveInputHistogramBin(c.x)], 1u);
    InterlockedAdd(group_counts[256u + CurveInputHistogramBin(c.y)], 1u);
    InterlockedAdd(group_counts[512u + CurveInputHistogramBin(c.z)], 1u);
  }
  GroupMemoryBarrierWithGroupSync();
  uint* counts = (uint*)push.counts;
  for (uint i = local_id.x; i < 768u; i += 256u) {
    const uint count = group_counts[i];
    if (count != 0u) {
      InterlockedAdd(counts[i], count);
    }
  }
}
