//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `drt` (src/edit/runtime/vulkan/shader/drt.slang): the display transform kernel
// of the OpenCL program opencl_dag_drt (drt.cl, with the ACES 2.0 and OpenDRT math of cst.cl) that
// the DRT pass dispatches (phase VK12).

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::drt_kernels {

/// Linear AP1 of a scene to the encoded display values (`drt_display_scene_rgba32f`).
inline constexpr std::string_view kDisplayScene       = "drt_display_scene_rgba32f";

/// Workgroup 16 x 16 x 1 (the OpenCL local size).
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image of a cached source; a dummy when the source is a buffer.
inline constexpr uint32_t         kSourceBinding      = 0;
/// The RGBA32F storage image of the display output; a dummy when the destination is a buffer.
inline constexpr uint32_t         kDestinationBinding = 1;

/// Push constants of `drt_display_scene_rgba32f` (scalar layout).
struct DisplayPushConstants {
  VkDeviceAddress source      = 0;  ///< RGBA32F row-major, or 0: the source image binding
  VkDeviceAddress destination = 0;  ///< RGBA32F row-major, or 0: the destination image binding
  VkDeviceAddress parameters  = 0;  ///< The block of vulkan_drt_layout.h
  uint32_t        width       = 0;
  uint32_t        height      = 0;
};
static_assert(sizeof(DisplayPushConstants) == 32);

[[nodiscard]] inline auto DisplaySceneDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kDisplayScene),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(DisplayPushConstants),
          {}};
}

/// The descriptors of the kernels of the group, for the kernel catalog.
[[nodiscard]] inline auto AllDescriptors() -> std::array<VulkanKernelDescriptor, 1> {
  return {DisplaySceneDescriptor()};
}

}  // namespace alcedo::vulkan::drt_kernels
