//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `lens_calib` (src/edit/runtime/lens/vulkan/shader/lens_calib.slang): the
// vignetting and the Lanczos-3 warp of the OpenCL program edit_geometry_lens_calib (phase VK5).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "edit/runtime/lens/lens_calib_runtime.hpp"
#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::lens_calib_kernels {

/// The vignetting gain of each texel, in place.
inline constexpr std::string_view kVignetting         = "edit_geometry_lens_vignetting_rgba32f";
/// Scale, projection, distortion, TCA, the Lanczos-3 sample, and the circle crop alpha.
inline constexpr std::string_view kWarp               = "edit_geometry_lens_warp_rgba32f";
/// Workgroup 16 x 16 x 1 for both kernels.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F image of the vignetting kernel (read and written) and the source of the warp.
inline constexpr uint32_t         kImageBinding       = 0;
/// The RGBA32F destination of the warp.
inline constexpr uint32_t         kDestinationBinding = 1;

// The Slang record has the layout of the host struct (scalar layout, a flat record).
static_assert(sizeof(LensCalibGpuParams) == 268);

/// Push constants of both kernels (scalar layout).
struct LensCalibPushConstants {
  VkDeviceAddress params = 0;  ///< one LensCalibGpuParams record
};
static_assert(sizeof(LensCalibPushConstants) == 8);

[[nodiscard]] inline auto VignettingDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kVignetting),
          {{kImageBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(LensCalibPushConstants),
          {}};
}

[[nodiscard]] inline auto WarpDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kWarp),
          {{kImageBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(LensCalibPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::lens_calib_kernels
