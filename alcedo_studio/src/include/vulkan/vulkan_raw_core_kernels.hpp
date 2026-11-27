//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `raw_core`
// (src/decoders/processor/operators/gpu/vulkan_shader/raw_core.slang): the linearization of an RGB
// input and of a U16 CFA plane, and the CFA clamp, the forms of the OpenCL to_linear_ref.cl
// kernels (phase VK4).

#pragma once

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::raw_core_kernels {

/// RGBA in place: subtract the black of each channel, clamp at 0 for integer codes, scale.
inline constexpr std::string_view kLinearizeRgb = "linearize_rgb";
/// U16 CFA samples to linear F32 samples, with the black, the white, and the relative white
/// balance of the color of each sample.
inline constexpr std::string_view kToLinearRef  = "to_linear_ref_u16_to_f32";
/// F32 CFA samples clamped to [0, 1] in place.
inline constexpr std::string_view kCfaClamp01   = "cfa_clamp01_f32";
/// Workgroup 16 x 16 x 1 for the three kernels.
inline constexpr uint32_t         kGroupSize    = 16;

/// `WBParams` of raw_utils_opencl.cl and raw_utils.slangh (scalar layout).
struct WBParams {
  float   black_level[4]      = {};
  float   white_level[4]      = {};
  float   wb_multipliers[4]   = {};
  int32_t apply_white_balance = 0;
  int32_t black_tile_width    = 0;
  int32_t black_tile_height   = 0;
  float   pattern_black[36]   = {};
};
static_assert(sizeof(WBParams) == 204);

/// `PatternParams` of raw_utils_opencl.cl and raw_utils.slangh (scalar layout).
struct PatternParams {
  int32_t width       = 0;
  int32_t height      = 0;
  int32_t tile_width  = 0;
  int32_t tile_height = 0;
  int32_t raw_fc[36]  = {};
};
static_assert(sizeof(PatternParams) == 160);

/// The parameter record of `to_linear_ref_u16_to_f32`, in a buffer: above the 128 push constant
/// bytes of every Vulkan device. The kernel reads the two structs as two records at the address
/// and at @ref kPatternParamsOffset (`kPatternParamsOffset` of raw_utils.slangh).
struct ToLinearRefParameters {
  WBParams      wb;
  PatternParams pattern;
};
static_assert(sizeof(ToLinearRefParameters) == 364);
inline constexpr uint64_t kPatternParamsOffset = 204;
static_assert(offsetof(ToLinearRefParameters, pattern) == kPatternParamsOffset);

/// Push constants of `linearize_rgb` (scalar layout).
struct LinearizeRgbPushConstants {
  VkDeviceAddress           rgba   = 0;  ///< width * height RGBA float values
  uint32_t                  width  = 0;
  uint32_t                  height = 0;
  RawRgbLinearizationParams params;
  uint32_t                  reserved = 0;  ///< pads the block to the 8-byte struct alignment
};
static_assert(sizeof(LinearizeRgbPushConstants) == 48);

/// Push constants of `to_linear_ref_u16_to_f32` (scalar layout).
struct ToLinearRefPushConstants {
  VkDeviceAddress source      = 0;  ///< width * height U16 samples, 2-byte aligned
  VkDeviceAddress destination = 0;  ///< width * height float values
  VkDeviceAddress parameters  = 0;  ///< one ToLinearRefParameters record
};
static_assert(sizeof(ToLinearRefPushConstants) == 24);

/// Push constants of `cfa_clamp01_f32` (scalar layout).
struct CfaClampPushConstants {
  VkDeviceAddress image  = 0;  ///< width * height float values
  uint32_t        width  = 0;
  uint32_t        height = 0;
};
static_assert(sizeof(CfaClampPushConstants) == 16);

[[nodiscard]] inline auto LinearizeRgbDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kLinearizeRgb), {}, sizeof(LinearizeRgbPushConstants), {}};
}

[[nodiscard]] inline auto ToLinearRefDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kToLinearRef), {}, sizeof(ToLinearRefPushConstants), {}};
}

[[nodiscard]] inline auto CfaClamp01Descriptor() -> VulkanKernelDescriptor {
  return {std::string(kCfaClamp01), {}, sizeof(CfaClampPushConstants), {}};
}

}  // namespace alcedo::vulkan::raw_core_kernels
