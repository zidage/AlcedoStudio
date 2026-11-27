//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group highlight_reconstruct: the Slang form of the opposed highlight reconstruction
// of opencl_shader/highlight_reconstruct.cl (plan phase VK5), on buffers at device addresses
// (plan section 4.2). The names and the push constant layouts are in
// include/vulkan/vulkan_highlight_kernels.hpp.
//
// Differences from the OpenCL program (VK5 record):
// - The clip mask and the dilated mask have one 32-bit word for each texel: bit 0 R, bit 1 G,
//   bit 2 B. The OpenCL masks have one byte for each channel; Vulkan has no 8-bit storage here.
// - The mask kernels write no clipped count: the OpenCL Develop pass never reads it.
// - The cube root of the reference mean is AlcedoPow (math.slangh): GLSL.std.450 pow is
//   exp2(y * log2(x)) and is less precise than the OpenCL pow.

#include "raw_pack.slangh"
#include "vulkan/shader/math.slangh"

// HighlightLimits: 36 bytes. The OpenCL HighlightParams without the chrominance (no caller kernel
// reads it) and with named fields in place of arrays (constant indices, section 4.2).
struct HighlightLimits {
  float clip_r;
  float clip_g;
  float clip_b;
  float clipdark_r;
  float clipdark_g;
  float clipdark_b;
  uint  width;   // the texels of the reconstructed region in a row (the crop width)
  uint  height;  // the rows of the reconstructed region
  uint  stride;  // the texels in a row of the source
};

// HighlightRgbaPushConstants: 80 bytes. hlr_dilate_mask also uses this layout.
struct HighlightRgbaPushConstants {
  float*          source;       // RGBA values, `stride` texels in a row
  float*          destination;  // RGBA values, `stride` texels in a row
  uint*           mask;         // width * height words
  uint*           dilated;      // width * height words
  uint*           statistics;   // sums R, G, B, counts R, G, B (float bits)
  HighlightLimits limits;
  uint            reserved;
};

// HighlightPlanarPushConstants: 96 bytes.
struct HighlightPlanarPushConstants {
  float*          r;  // planes of `stride` values in a row
  float*          g;
  float*          b;
  uint*           mask;
  uint*           dilated;
  uint*           statistics;
  HighlightLimits limits;
  uint            crop_x;
  uint            crop_y;
  uint            reserved;
};

// HighlightPlanarPackPushConstants: 120 bytes.
struct HighlightPlanarPackPushConstants {
  float*           r;
  float*           g;
  float*           b;
  uint*            statistics;
  HighlightLimits  limits;
  PackOrientParams pack;
};

static const uint  kMaskR        = 1u;
static const uint  kMaskG        = 2u;
static const uint  kMaskB        = 4u;
static const uint  kMaskRgb      = 7u;
static const int   kDilateRadius = 3;
// Reconstruction ramps in over the top (1 - kSoftClipLo) fraction below the clip level (the
// OpenCL rule: a hard threshold gives speckle at highlight edges).
static const float kSoftClipLo   = 0.95f;
// A channel needs more than this many chrominance samples for a chrominance correction.
static const float kMinChromaSamples = 30.0f;
static const uint  kStatisticsSums   = 0u;
static const uint  kStatisticsCounts = 3u;

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination;

static float Cube(float value) {
  return value * value * value;
}

// A finite, non-negative readout, or 0.
static float SanitizeChannel(float value) {
  return isfinite(value) ? max(value, 0.0f) : 0.0f;
}

static float4 MaxRgb(float4 value) {
  return float4(SanitizeChannel(value.x), SanitizeChannel(value.y), SanitizeChannel(value.z),
                value.w);
}

static float SoftClipWeight(float value, float clip) {
  const float lo = kSoftClipLo * clip;
  const float t  = clamp((value - lo) / (clip - lo), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static float ReconstructChannel(float pixel, float ref, float chrominance, float weight) {
  if (weight <= 0.0f) {
    return pixel;
  }
  return pixel + weight * (max(pixel, ref + chrominance) - pixel);
}

static float4 LoadRgba(float* rgba, uint index) {
  const uint base = index * 4u;
  return float4(rgba[base], rgba[base + 1u], rgba[base + 2u], rgba[base + 3u]);
}

static float4 LoadPlanarRgb(float* r, float* g, float* b, uint row, uint col, uint stride) {
  const uint index = row * stride + col;
  return float4(r[index], g[index], b[index], 1.0f);
}

// The sums of the 3 x 3 neighbourhood: a sample at or above the clip level is censored and feeds
// only the fallback mean (the OpenCL RefavgAccumulate).
struct RefavgSums {
  float3 valid_sum;
  float3 valid_count;
  float3 all_sum;
  float3 all_count;
};

static void AccumulateChannel(inout float valid_sum, inout float valid_count, inout float all_sum,
                              inout float all_count, float raw, float clip) {
  if (isfinite(raw)) {
    const float v = max(raw, 0.0f);
    all_sum += v;
    all_count += 1.0f;
    if (v < clip) {
      valid_sum += v;
      valid_count += 1.0f;
    }
  }
}

static void RefavgAccumulate(inout RefavgSums sums, float4 raw, HighlightLimits limits) {
  AccumulateChannel(sums.valid_sum.x, sums.valid_count.x, sums.all_sum.x, sums.all_count.x, raw.x,
                    limits.clip_r);
  AccumulateChannel(sums.valid_sum.y, sums.valid_count.y, sums.all_sum.y, sums.all_count.y, raw.y,
                    limits.clip_g);
  AccumulateChannel(sums.valid_sum.z, sums.valid_count.z, sums.all_sum.z, sums.all_count.z, raw.z,
                    limits.clip_b);
}

static float CubeRootOfMean(float valid_sum, float valid_count, float all_sum, float all_count) {
  const float m = (valid_count > 0.0f) ? valid_sum / valid_count
                  : (all_count > 0.0f) ? all_sum / all_count
                                       : 0.0f;
  return m > 0.0f ? AlcedoPow(m, 1.0f / 3.0f) : 0.0f;
}

static float4 RefavgFromSums(RefavgSums sums) {
  const float mean_r =
      CubeRootOfMean(sums.valid_sum.x, sums.valid_count.x, sums.all_sum.x, sums.all_count.x);
  const float mean_g =
      CubeRootOfMean(sums.valid_sum.y, sums.valid_count.y, sums.all_sum.y, sums.all_count.y);
  const float mean_b =
      CubeRootOfMean(sums.valid_sum.z, sums.valid_count.z, sums.all_sum.z, sums.all_count.z);
  return float4(Cube(0.5f * (mean_g + mean_b)), Cube(0.5f * (mean_r + mean_b)),
                Cube(0.5f * (mean_r + mean_g)), 0.0f);
}

static RefavgSums EmptySums() {
  RefavgSums sums;
  sums.valid_sum   = float3(0.0f);
  sums.valid_count = float3(0.0f);
  sums.all_sum     = float3(0.0f);
  sums.all_count   = float3(0.0f);
  return sums;
}

// The reference of an RGBA source texel (row, col) of the region.
static float4 CalcRefavg(float* source, int row, int col, HighlightLimits limits) {
  RefavgSums sums  = EmptySums();
  const int  dymin = max(0, row - 1);
  const int  dxmin = max(0, col - 1);
  const int  dymax = min(int(limits.height) - 1, row + 1);
  const int  dxmax = min(int(limits.width) - 1, col + 1);
  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(sums, LoadRgba(source, uint(dy) * limits.stride + uint(dx)), limits);
    }
  }
  return RefavgFromSums(sums);
}

// The reference of crop texel (local_row, local_col) of planes with the crop at (crop_x, crop_y).
static float4 CalcRefavgPlanar(float* r, float* g, float* b, int local_row, int local_col,
                               HighlightLimits limits, uint crop_x, uint crop_y) {
  RefavgSums sums  = EmptySums();
  const int  dymin = max(0, local_row - 1);
  const int  dxmin = max(0, local_col - 1);
  const int  dymax = min(int(limits.height) - 1, local_row + 1);
  const int  dxmax = min(int(limits.width) - 1, local_col + 1);
  for (int dy = dymin; dy <= dymax; ++dy) {
    for (int dx = dxmin; dx <= dxmax; ++dx) {
      RefavgAccumulate(
          sums, LoadPlanarRgb(r, g, b, crop_y + uint(dy), crop_x + uint(dx), limits.stride),
          limits);
    }
  }
  return RefavgFromSums(sums);
}

static uint ClipBits(float4 pixel, HighlightLimits limits) {
  return (pixel.x >= limits.clip_r ? kMaskR : 0u) | (pixel.y >= limits.clip_g ? kMaskG : 0u) |
         (pixel.z >= limits.clip_b ? kMaskB : 0u);
}

// The OpenCL AtomicAddFloat: a compare-and-swap loop on the float bits. The first guess is 0 so
// that the loop reads the word only through the atomic operation.
static void AtomicAddFloat(uint* address, float value) {
  uint expected = 0u;
  while (true) {
    const uint desired = asuint(asfloat(expected) + value);
    uint       original;
    InterlockedCompareExchange(*address, expected, desired, original);
    if (original == expected) {
      return;
    }
    expected = original;
  }
}

groupshared float l_contrib_r[256];
groupshared float l_contrib_g[256];
groupshared float l_contrib_b[256];
groupshared float l_count_r[256];
groupshared float l_count_g[256];
groupshared float l_count_b[256];

// The workgroup sum of the chrominance contributions, then one atomic add for each value (the
// OpenCL reduction). Every invocation of the workgroup calls it.
static void AddChrominance(uint lid, float3 contrib, float3 count, uint* statistics) {
  l_contrib_r[lid] = contrib.x;
  l_contrib_g[lid] = contrib.y;
  l_contrib_b[lid] = contrib.z;
  l_count_r[lid]   = count.x;
  l_count_g[lid]   = count.y;
  l_count_b[lid]   = count.z;
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (lid < stride) {
      l_contrib_r[lid] += l_contrib_r[lid + stride];
      l_contrib_g[lid] += l_contrib_g[lid + stride];
      l_contrib_b[lid] += l_contrib_b[lid + stride];
      l_count_r[lid] += l_count_r[lid + stride];
      l_count_g[lid] += l_count_g[lid + stride];
      l_count_b[lid] += l_count_b[lid + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lid == 0u) {
    AtomicAddFloat(statistics + kStatisticsSums + 0u, l_contrib_r[0]);
    AtomicAddFloat(statistics + kStatisticsSums + 1u, l_contrib_g[0]);
    AtomicAddFloat(statistics + kStatisticsSums + 2u, l_contrib_b[0]);
    AtomicAddFloat(statistics + kStatisticsCounts + 0u, l_count_r[0]);
    AtomicAddFloat(statistics + kStatisticsCounts + 1u, l_count_g[0]);
    AtomicAddFloat(statistics + kStatisticsCounts + 2u, l_count_b[0]);
  }
}

// The contribution of one texel inside the chrominance ring (dilated mask, above the dark limit,
// below the clip level).
static void Contribution(float4 pixel, uint dilated, float4 ref_value, HighlightLimits limits,
                         inout float3 contrib, inout float3 count) {
  if ((dilated & kMaskR) != 0u && pixel.x > limits.clipdark_r && pixel.x < limits.clip_r) {
    contrib.x = pixel.x - ref_value.x;
    count.x   = 1.0f;
  }
  if ((dilated & kMaskG) != 0u && pixel.y > limits.clipdark_g && pixel.y < limits.clip_g) {
    contrib.y = pixel.y - ref_value.y;
    count.y   = 1.0f;
  }
  if ((dilated & kMaskB) != 0u && pixel.z > limits.clipdark_b && pixel.z < limits.clip_b) {
    contrib.z = pixel.z - ref_value.z;
    count.z   = 1.0f;
  }
}

static bool InChromaRing(float4 pixel, uint dilated, HighlightLimits limits) {
  return ((dilated & kMaskR) != 0u && pixel.x > limits.clipdark_r && pixel.x < limits.clip_r) ||
         ((dilated & kMaskG) != 0u && pixel.y > limits.clipdark_g && pixel.y < limits.clip_g) ||
         ((dilated & kMaskB) != 0u && pixel.z > limits.clipdark_b && pixel.z < limits.clip_b);
}

static float ChrominanceOf(uint* statistics, uint channel) {
  const float count = asfloat(statistics[kStatisticsCounts + channel]);
  return count > kMinChromaSamples ? asfloat(statistics[kStatisticsSums + channel]) / count : 0.0f;
}

static float4 Reconstruct(float4 pixel, float4 ref_value, float3 chrominance, float3 weight) {
  return float4(ReconstructChannel(pixel.x, ref_value.x, chrominance.x, weight.x),
                ReconstructChannel(pixel.y, ref_value.y, chrominance.y, weight.y),
                ReconstructChannel(pixel.z, ref_value.z, chrominance.z, weight.z), pixel.w);
}

static float3 SoftClipWeights(float4 pixel, HighlightLimits limits) {
  return float3(SoftClipWeight(pixel.x, limits.clip_r), SoftClipWeight(pixel.y, limits.clip_g),
                SoftClipWeight(pixel.z, limits.clip_b));
}

static float3 Chrominance(uint* statistics) {
  return float3(ChrominanceOf(statistics, 0u), ChrominanceOf(statistics, 1u),
                ChrominanceOf(statistics, 2u));
}

static bool AnyWeight(float3 weight) {
  return weight.x > 0.0f || weight.y > 0.0f || weight.z > 0.0f;
}

// ---- RGBA source ---------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_build_mask(uint3 id: SV_DispatchThreadID, uniform HighlightRgbaPushConstants push) {
  const HighlightLimits limits = push.limits;
  if (id.x >= limits.width || id.y >= limits.height) {
    return;
  }
  const float4 pixel = MaxRgb(LoadRgba(push.source, id.y * limits.stride + id.x));
  push.mask[id.y * limits.width + id.x] = ClipBits(pixel, limits);
}

// The 7 x 7 dilation of each channel bit (the OpenCL DilateMaskAt), clamped at the region border.
[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_dilate_mask(uint3 id: SV_DispatchThreadID, uniform HighlightRgbaPushConstants push) {
  const HighlightLimits limits = push.limits;
  if (id.x >= limits.width || id.y >= limits.height) {
    return;
  }
  const int row = int(id.y);
  const int col = int(id.x);
  const int y0  = max(0, row - kDilateRadius);
  const int x0  = max(0, col - kDilateRadius);
  const int y1  = min(int(limits.height) - 1, row + kDilateRadius);
  const int x1  = min(int(limits.width) - 1, col + kDilateRadius);
  uint      bits = 0u;
  for (int y = y0; y <= y1 && bits != kMaskRgb; ++y) {
    for (int x = x0; x <= x1; ++x) {
      bits |= push.mask[uint(y) * limits.width + uint(x)];
    }
  }
  push.dilated[id.y * limits.width + id.x] = bits;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_chrominance_contrib(uint3 id: SV_DispatchThreadID, uint3 local_id: SV_GroupThreadID,
                             uniform HighlightRgbaPushConstants push) {
  const HighlightLimits limits  = push.limits;
  float3                contrib = float3(0.0f);
  float3                count   = float3(0.0f);
  if (id.x < limits.width && id.y < limits.height) {
    const float4 pixel   = MaxRgb(LoadRgba(push.source, id.y * limits.stride + id.x));
    const uint   dilated = push.dilated[id.y * limits.width + id.x];
    // The reference costs nine reads; only texels inside the chrominance ring pay for it.
    if (InChromaRing(pixel, dilated, limits)) {
      const float4 ref_value = CalcRefavg(push.source, int(id.y), int(id.x), limits);
      Contribution(pixel, dilated, ref_value, limits, contrib, count);
    }
  }
  AddChrominance(local_id.y * 16u + local_id.x, contrib, count, push.statistics);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_reconstruct_from_stats(uint3 id: SV_DispatchThreadID,
                                uniform HighlightRgbaPushConstants push) {
  const HighlightLimits limits = push.limits;
  if (id.x >= limits.width || id.y >= limits.height) {
    return;
  }
  const uint   index       = id.y * limits.stride + id.x;
  const float4 input_pixel = LoadRgba(push.source, index);
  const float4 pixel       = MaxRgb(input_pixel);
  const float3 weight      = SoftClipWeights(pixel, limits);
  float4       result      = pixel;
  // The reference costs nine reads; unclipped texels keep their value and never read it.
  if (AnyWeight(weight)) {
    const float4 ref_value = CalcRefavg(push.source, int(id.y), int(id.x), limits);
    result                 = Reconstruct(pixel, ref_value, Chrominance(push.statistics), weight);
  }
  const uint base              = index * 4u;
  push.destination[base]      = result.x;
  push.destination[base + 1u] = result.y;
  push.destination[base + 2u] = result.z;
  push.destination[base + 3u] = input_pixel.w;
}

// ---- Planar source (the RCD planes), with the crop ----------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_build_mask_planar(uint3 id: SV_DispatchThreadID,
                           uniform HighlightPlanarPushConstants push) {
  const HighlightLimits limits = push.limits;
  if (id.x >= limits.width || id.y >= limits.height) {
    return;
  }
  const float4 pixel = MaxRgb(
      LoadPlanarRgb(push.r, push.g, push.b, push.crop_y + id.y, push.crop_x + id.x, limits.stride));
  push.mask[id.y * limits.width + id.x] = ClipBits(pixel, limits);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_chrominance_contrib_planar(uint3 id: SV_DispatchThreadID,
                                    uint3 local_id: SV_GroupThreadID,
                                    uniform HighlightPlanarPushConstants push) {
  const HighlightLimits limits  = push.limits;
  float3                contrib = float3(0.0f);
  float3                count   = float3(0.0f);
  if (id.x < limits.width && id.y < limits.height) {
    const float4 pixel   = MaxRgb(LoadPlanarRgb(push.r, push.g, push.b, push.crop_y + id.y,
                                                push.crop_x + id.x, limits.stride));
    const uint   dilated = push.dilated[id.y * limits.width + id.x];
    if (InChromaRing(pixel, dilated, limits)) {
      const float4 ref_value = CalcRefavgPlanar(push.r, push.g, push.b, int(id.y), int(id.x),
                                                limits, push.crop_x, push.crop_y);
      Contribution(pixel, dilated, ref_value, limits, contrib, count);
    }
  }
  AddChrominance(local_id.y * 16u + local_id.x, contrib, count, push.statistics);
}

// The reconstruction of the crop, then the pack with the inverse white balance and the
// orientation into the RGBA32F image (binding 0); alpha 1.
[shader("compute")]
[numthreads(16, 16, 1)]
void hlr_reconstruct_from_stats_planar_pack(uint3 id: SV_DispatchThreadID,
                                            uniform HighlightPlanarPackPushConstants push) {
  const HighlightLimits  limits = push.limits;
  const PackOrientParams pack   = push.pack;
  if (id.x >= pack.src_width || id.y >= pack.src_height) {
    return;
  }
  const float4 pixel  = MaxRgb(LoadPlanarRgb(push.r, push.g, push.b, pack.src_y + id.y,
                                             pack.src_x + id.x, pack.plane_stride));
  const float3 weight = SoftClipWeights(pixel, limits);
  float4       result = pixel;
  if (AnyWeight(weight)) {
    const float4 ref_value = CalcRefavgPlanar(push.r, push.g, push.b, int(id.y), int(id.x), limits,
                                              pack.src_x, pack.src_y);
    result                 = Reconstruct(pixel, ref_value, Chrominance(push.statistics), weight);
  }
  destination[OrientedCoord(id.x, id.y, pack)] =
      float4(result.x * pack.scale_r, result.y * pack.scale_g, result.z * pack.scale_b, 1.0f);
}
