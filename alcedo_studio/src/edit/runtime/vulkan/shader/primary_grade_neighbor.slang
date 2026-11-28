//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group primary_grade_neighbor (plan #1036, phase VK9): the separable neighborhood
// kernels of opencl/shader/primary_grade_neighbor.cl that the product dispatches,
// primary_grade_neighbor_blur_h_scene_rgba32f and primary_grade_neighbor_apply_v_scene_rgba32f.
// The DRT finish runs Clarity, Sharpen, and Halation with them. The names, the bindings, and the
// push constant layouts are in include/vulkan/vulkan_primary_grade_neighbor_kernels.hpp.
//
// A scene image is a cached RGBA32F storage image or a scene-work member (an RGBA32F row-major
// buffer). A kernel reads the buffer when its address is not 0, else the image of its binding.
// GradeNeighborParams (adjustment_runtime.hpp, 300 bytes) is read by address arithmetic: a
// pointer to a record struct crashes the Intel compiler (phase VK4 record).

#include "vulkan/shader/math.slangh"

[[vk::binding(0, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> source_image;
[[vk::binding(1, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> blur_image;
[[vk::binding(2, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> destination_image;
[[vk::binding(3, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> mix_image;
[[vk::binding(4, 0)]] [vk::image_format("r8")] RWTexture2D<float> mask_image;

// NeighborBlurPushConstants: 24 bytes.
struct NeighborBlurPushConstants {
  uint64_t params;  // GradeNeighborParams
  uint64_t source;  // float4 row-major, or 0: source_image
  uint     width;
  uint     height;
};

// NeighborApplyPushConstants: 64 bytes.
struct NeighborApplyPushConstants {
  uint64_t params;       // GradeNeighborParams
  uint64_t source;       // float4 row-major, or 0: source_image
  uint64_t destination;  // float4 row-major, or 0: destination_image
  uint64_t mix_source;   // float4 row-major, or 0: mix_image
  uint     width;
  uint     height;
  uint     apply_mix;  // 0: no mix; 1: the mix with the mix source
  uint     has_mask;   // 1: the mix is multiplied by mask_image
  float    grade_mix;
  uint     pad[3];
};

// Byte layout of GradeNeighborParams.
static const uint kBehaviorOffset  = 0u;
static const uint kRadiusOffset    = 4u;
static const uint kTapCountOffset  = 8u;
static const uint kAmountOffset    = 12u;
static const uint kThresholdOffset = 16u;
static const uint kWeightsOffset   = 20u;
static const uint kSigmaXOffset    = 280u;
static const uint kSigmaYOffset    = 284u;
static const uint kRedshiftOffset  = 288u;

static const uint kBehaviorClarity  = 13u;
static const uint kBehaviorSharpen  = 14u;
static const uint kBehaviorHalation = 15u;

// GRADE_NEIGHBOR_MAX_TAPS - 1: the largest vertical radius, which sizes the workgroup tile.
static const int  kMaxRadius = 63;
static const uint kGroupEdge = 8u;
static const uint kTileCount = kGroupEdge * (kGroupEdge + 2u * uint(kMaxRadius));

static uint  ParamUint(uint64_t params, uint offset) { return *((uint*)(params + offset)); }
static float ParamFloat(uint64_t params, uint offset) { return *((float*)(params + offset)); }
static float Weight(uint64_t params, uint tap) {
  return ParamFloat(params, kWeightsOffset + tap * 4u);
}

static float4* Float4At(uint64_t address) { return (float4*)address; }

static float GradeNeighborLuma(float3 value) {
  return value.x * 0.114f + value.y * 0.587f + value.z * 0.299f;
}

static float GradeNeighborSmoothstep(float edge0, float edge1, float value) {
  const float t = clamp((value - edge0) / max(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static int GradeHalationRadius(float sigma) {
  if (!(sigma > 0.0f)) {
    return 0;
  }
  return clamp(int(ceil(sigma * 3.0f)), 1, kMaxRadius);
}

static float GradeHalationWeight(int tap, float sigma) {
  return tap == 0 ? 1.0f : AlcedoExp(-float(tap) / max(sigma, 1.0e-6f));
}

static float GradeHalationNormalization(int radius, float sigma) {
  float sum = 1.0f;
  for (int tap = 1; tap <= radius; ++tap) {
    sum += 2.0f * GradeHalationWeight(tap, sigma);
  }
  return 1.0f / max(sum, 1.0e-6f);
}

static int GradeVerticalRadius(uint64_t params) {
  if (ParamUint(params, kBehaviorOffset) == kBehaviorHalation) {
    return GradeHalationRadius(ParamFloat(params, kSigmaYOffset));
  }
  return int(ParamUint(params, kRadiusOffset));
}

// A texel of a scene image with the coordinate clamped to the image: SceneTap of the OpenCL file.
static float4 SceneTap(uint64_t buffer, RWTexture2D<float4> image, int2 coord, uint width,
                       uint height) {
  const uint x = uint(clamp(coord.x, 0, int(width) - 1));
  const uint y = uint(clamp(coord.y, 0, int(height) - 1));
  if (buffer != 0u) {
    return Float4At(buffer)[y * width + x];
  }
  return image[uint2(x, y)];
}

// The horizontal blur of the source into blur_image: the Gaussian weights of the parameters, or
// the exponential window of the Halation (sigma_x).
[shader("compute")]
[numthreads(8, 8, 1)]
void primary_grade_neighbor_blur_h_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                                                 uniform NeighborBlurPushConstants push) {
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const int2 gid = int2(id.xy);
  float4     result;
  if (ParamUint(push.params, kBehaviorOffset) == kBehaviorHalation) {
    const float sigma  = ParamFloat(push.params, kSigmaXOffset);
    const int   radius = GradeHalationRadius(sigma);
    const float norm   = GradeHalationNormalization(radius, sigma);
    result = SceneTap(push.source, source_image, gid, push.width, push.height) * norm;
    for (int tap = 1; tap <= radius; ++tap) {
      const float weight = GradeHalationWeight(tap, sigma) * norm;
      result += (SceneTap(push.source, source_image, gid + int2(tap, 0), push.width, push.height) +
                 SceneTap(push.source, source_image, gid - int2(tap, 0), push.width, push.height)) *
                weight;
    }
  } else {
    result = SceneTap(push.source, source_image, gid, push.width, push.height) *
             Weight(push.params, 0u);
    const uint tap_count = ParamUint(push.params, kTapCountOffset);
    for (uint tap = 1u; tap < tap_count; ++tap) {
      const int distance = int(tap);
      result += (SceneTap(push.source, source_image, gid + int2(distance, 0), push.width,
                          push.height) +
                 SceneTap(push.source, source_image, gid - int2(distance, 0), push.width,
                          push.height)) *
                Weight(push.params, tap);
    }
  }
  blur_image[id.xy] = result;
}

// The red, green, and blue of the horizontal result in three planes (12 864 bytes, inside the
// Vulkan minimum of 16 KiB): every adjustment reads only the color of the vertical blur.
groupshared float vertical_tile_r[kTileCount];
groupshared float vertical_tile_g[kTileCount];
groupshared float vertical_tile_b[kTileCount];

static float3 TileAt(int index) {
  return float3(vertical_tile_r[index], vertical_tile_g[index], vertical_tile_b[index]);
}

static float3 GradeGaussianVertical(int center, uint64_t params) {
  float3     blur      = TileAt(center) * Weight(params, 0u);
  const uint tap_count = ParamUint(params, kTapCountOffset);
  for (uint tap = 1u; tap < tap_count; ++tap) {
    const int distance = int(tap) * int(kGroupEdge);
    blur += (TileAt(center + distance) + TileAt(center - distance)) * Weight(params, tap);
  }
  return blur;
}

static float3 GradeHalationVertical(int center, uint64_t params) {
  const float sigma  = ParamFloat(params, kSigmaYOffset);
  const int   radius = GradeHalationRadius(sigma);
  const float norm   = GradeHalationNormalization(radius, sigma);
  float3      blur   = TileAt(center) * norm;
  for (int tap = 1; tap <= radius; ++tap) {
    const float weight = GradeHalationWeight(tap, sigma) * norm;
    blur += (TileAt(center - tap * int(kGroupEdge)) + TileAt(center + tap * int(kGroupEdge))) *
            weight;
  }
  return blur;
}

// The vertical blur of blur_image in the workgroup tile, then Sharpen, Clarity, or Halation on the
// source, then the optional (masked) mix with the mix source. The host sends no other behavior
// (RequireVulkanNeighborhoodKernelBehavior) and no vertical radius above kMaxRadius.
[shader("compute")]
[numthreads(8, 8, 1)]
void primary_grade_neighbor_apply_v_scene_rgba32f(uint3 id: SV_DispatchThreadID,
                                                  uint3 group: SV_GroupID,
                                                  uint3 local_id: SV_GroupThreadID,
                                                  uniform NeighborApplyPushConstants push) {
  const int  radius      = GradeVerticalRadius(push.params);
  const uint tile_height = kGroupEdge + 2u * uint(radius);
  const uint local_index = local_id.y * kGroupEdge + local_id.x;
  for (uint tile_index = local_index; tile_index < kGroupEdge * tile_height;
       tile_index += kGroupEdge * kGroupEdge) {
    const uint tile_x   = tile_index % kGroupEdge;
    const uint tile_y   = tile_index / kGroupEdge;
    const uint source_x = group.x * kGroupEdge + tile_x;
    const int  source_y =
        clamp(int(group.y * kGroupEdge + tile_y) - radius, 0, int(push.height) - 1);
    const float4 value         = source_x < push.width
                                     ? blur_image[uint2(source_x, uint(source_y))]
                                     : float4(0.0f);
    vertical_tile_r[tile_index] = value.x;
    vertical_tile_g[tile_index] = value.y;
    vertical_tile_b[tile_index] = value.z;
  }
  GroupMemoryBarrierWithGroupSync();
  if (id.x >= push.width || id.y >= push.height) {
    return;
  }
  const int2   gid    = int2(id.xy);
  const float4 source = SceneTap(push.source, source_image, gid, push.width, push.height);
  const int    center = int((local_id.y + uint(radius)) * kGroupEdge + local_id.x);
  const uint   behavior = ParamUint(push.params, kBehaviorOffset);
  const float  amount   = ParamFloat(push.params, kAmountOffset);
  float4       adjusted = source;
  if (behavior == kBehaviorSharpen) {
    const float3 blur      = GradeGaussianVertical(center, push.params);
    float3       high      = source.xyz - blur;
    const float  threshold = ParamFloat(push.params, kThresholdOffset);
    if (threshold > 0.0f && abs(GradeNeighborLuma(high)) <= threshold) {
      high = float3(0.0f);
    }
    adjusted = float4(source.xyz + high * amount, source.w);
  } else if (behavior == kBehaviorClarity) {
    const float3 blur    = GradeGaussianVertical(center, push.params);
    const float3 diff    = source.xyz - blur;
    const float  protect = 1.0f - GradeNeighborSmoothstep(0.0f, 0.18f, abs(GradeNeighborLuma(diff)));
    const float  centered = (GradeNeighborLuma(source.xyz) - 0.5f) * 2.0f;
    const float  strength = amount * protect * max(1.0f - centered * centered, 0.0f);
    adjusted = float4(fma(diff, float3(strength), source.xyz), source.w);
  } else if (behavior == kBehaviorHalation) {
    const float3 blur     = GradeHalationVertical(center, push.params);
    const float3 spill    = max(blur - source.xyz, float3(0.0f));
    const float3 redshift = float3(ParamFloat(push.params, kRedshiftOffset),
                                   ParamFloat(push.params, kRedshiftOffset + 4u),
                                   ParamFloat(push.params, kRedshiftOffset + 8u));
    adjusted = float4(source.xyz + spill * amount * redshift, source.w);
  }
  if (push.apply_mix != 0u) {
    const float4 original = SceneTap(push.mix_source, mix_image, gid, push.width, push.height);
    float        mix      = push.grade_mix;
    if (push.has_mask != 0u) {
      mix *= mask_image[id.xy];
    }
    mix      = clamp(mix, 0.0f, 1.0f);
    adjusted = float4(original.xyz + (adjusted.xyz - original.xyz) * mix, original.w);
  }
  if (push.destination != 0u) {
    Float4At(push.destination)[id.y * push.width + id.x] = adjusted;
  } else {
    destination_image[id.xy] = adjusted;
  }
}
