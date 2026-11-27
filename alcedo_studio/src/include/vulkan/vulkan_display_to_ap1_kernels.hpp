//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `display_to_ap1` (src/edit/runtime/vulkan/shader/display_to_ap1.slang): the
// raster input kernels of the OpenCL program opencl_dag_display_to_ap1 (phase VK7).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::display_to_ap1_kernels {

/// U8, U16, or F32 RGBA raster samples to display-linear (or scene-linear) RGBA32F values.
inline constexpr std::string_view kLinearizeRaster    = "linearize_raster_rgba";
/// Source RGB to ACEScc AP1 (`D2aSourceToAcesccAp1`).
inline constexpr std::string_view kDisplayToAp1       = "display_to_ap1_acescc";
/// Display RGB to linear AP0 by the ACES 2.0 inverse alone (`A2rDisplayToAp0`), for the tests.
inline constexpr std::string_view kDisplayToAp0Linear = "display_to_ap0_linear";
/// Workgroup 16 x 16 x 1 for the three kernels.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image that the DisplayToAp1 kernels read (set 0).
inline constexpr uint32_t         kSourceBinding      = 0;
/// The RGBA32F storage image that the DisplayToAp1 kernels write (set 0).
inline constexpr uint32_t         kDestinationBinding = 1;

/// Push constants of `linearize_raster_rgba` (scalar layout).
struct LinearizeRasterPushConstants {
  VkDeviceAddress source      = 0;  ///< width * height RGBA samples of `format`, 4-byte aligned
  VkDeviceAddress parameters  = 0;  ///< the `PackRasterLinearize` block
  VkDeviceAddress destination = 0;  ///< width * height RGBA32F values
  uint32_t        format      = 0;  ///< `HostPixelFormat`: 1 F32, 2 U8, 3 U16
  uint32_t        width       = 0;
  uint32_t        height      = 0;
  uint32_t        reserved    = 0;  ///< pads the block to the 8-byte struct alignment
};
static_assert(sizeof(LinearizeRasterPushConstants) == 40);

/// Push constants of `display_to_ap1_acescc` and `display_to_ap0_linear` (scalar layout).
struct DisplayToAp1PushConstants {
  VkDeviceAddress parameters = 0;  ///< the DisplayToAp1 block (`display_to_ap1_math.h` layout)
  uint32_t        width      = 0;
  uint32_t        height     = 0;
};
static_assert(sizeof(DisplayToAp1PushConstants) == 16);

[[nodiscard]] inline auto LinearizeRasterDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kLinearizeRaster), {}, sizeof(LinearizeRasterPushConstants), {}};
}

/// @p kernel: @ref kDisplayToAp1 or @ref kDisplayToAp0Linear.
[[nodiscard]] inline auto DisplayToAp1Descriptor(std::string_view kernel)
    -> VulkanKernelDescriptor {
  return {std::string(kernel),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(DisplayToAp1PushConstants),
          {}};
}

}  // namespace alcedo::vulkan::display_to_ap1_kernels
