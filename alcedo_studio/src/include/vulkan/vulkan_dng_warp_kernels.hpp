//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `dng_warp` (src/edit/runtime/vulkan/shader/dng_warp.slang): the DNG
// OpcodeList3 WarpRectilinear kernel of the OpenCL program opencl_dag_geometry_camera (phase VK5).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::dng_warp_kernels {

/// The warp of an RGBA32F image with the coefficient set of each color plane.
inline constexpr std::string_view kWarpRectilinear    = "warp_rectilinear_rgba32f";
/// Workgroup 16 x 16 x 1.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image that the kernel reads (set 0).
inline constexpr uint32_t         kSourceBinding      = 0;
/// The RGBA32F storage image that the kernel writes (set 0).
inline constexpr uint32_t         kDestinationBinding = 1;

/// Push constants of `warp_rectilinear_rgba32f` (scalar layout): the OpenCL
/// `OpenClImageWarpParams`, with the three coefficient sets as three arrays.
struct WarpRectilinearPushConstants {
  uint32_t coefficient_set_count = 0;
  uint32_t width                 = 0;
  uint32_t height                = 0;
  float    set_r[6]              = {};  ///< plane 0, and every plane with one set
  float    set_g[6]              = {};
  float    set_b[6]              = {};
  float    center_x              = 0.5F;
  float    center_y              = 0.5F;
};
static_assert(sizeof(WarpRectilinearPushConstants) == 92);

[[nodiscard]] inline auto WarpRectilinearDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kWarpRectilinear),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(WarpRectilinearPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::dng_warp_kernels
