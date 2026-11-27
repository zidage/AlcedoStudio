//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `xtrans_interpolate`
// (src/decoders/processor/operators/gpu/vulkan_shader/xtrans_interpolate.slang): the X-Trans
// demosaic of the OpenCL xtrans_interpolate.cl (phase VK4).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::xtrans_kernels {

/// The green plane: the CFA green, or the directional green estimate.
inline constexpr std::string_view kGreen     = "xtrans_green";
/// RGBA from the CFA and the green plane.
inline constexpr std::string_view kRgba      = "xtrans_rgba";
/// Workgroup 16 x 16 x 1 for both kernels.
inline constexpr uint32_t         kGroupSize = 16;

/// `XTransParams` of xtrans_interpolate.cl (scalar layout). The record is in a buffer: above the
/// 128 push constant bytes of every Vulkan device.
struct XTransParams {
  uint32_t width        = 0;
  uint32_t height       = 0;
  uint32_t tile_width   = 6;
  uint32_t tile_height  = 6;
  uint32_t passes       = 1;
  uint32_t green_radius = 3;
  uint32_t rb_radius    = 3;
  uint32_t rgb_fc[36]   = {};
};
static_assert(sizeof(XTransParams) == 172);

/// Push constants of both kernels (scalar layout).
struct XTransPushConstants {
  VkDeviceAddress raw        = 0;  ///< width * height linear CFA values
  VkDeviceAddress green      = 0;  ///< width * height values
  VkDeviceAddress rgba       = 0;  ///< width * height RGBA values; `xtrans_rgba` only
  VkDeviceAddress parameters = 0;  ///< one XTransParams record
};
static_assert(sizeof(XTransPushConstants) == 32);

[[nodiscard]] inline auto GreenDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kGreen), {}, sizeof(XTransPushConstants), {}};
}

[[nodiscard]] inline auto RgbaDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kRgba), {}, sizeof(XTransPushConstants), {}};
}

}  // namespace alcedo::vulkan::xtrans_kernels
