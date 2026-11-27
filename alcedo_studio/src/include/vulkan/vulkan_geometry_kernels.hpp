//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `geometry_utils` (src/vulkan/shader/geometry_utils.slang): crop and bilinear
// resize of RGBA32F images, the form of the OpenCL geometry_utils.cl kernel (phase VK3).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::geometry_kernels {

/// Bilinear sample of the crop at the center of each destination texel.
inline constexpr std::string_view kCropResizeLinear   = "geometry_crop_resize_linear";
/// Workgroup 16 x 16 x 1.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image that the kernel reads (set 0).
inline constexpr uint32_t         kSourceBinding      = 0;
/// The RGBA32F storage image that the kernel writes (set 0).
inline constexpr uint32_t         kDestinationBinding = 1;

/// Push constants of `geometry_crop_resize_linear` (scalar layout).
struct CropResizePushConstants {
  int32_t source_width       = 0;
  int32_t source_height      = 0;
  int32_t crop_x             = 0;
  int32_t crop_y             = 0;
  int32_t crop_width         = 0;
  int32_t crop_height        = 0;
  int32_t destination_width  = 0;
  int32_t destination_height = 0;
  float   scale_x            = 1.0F;  ///< crop width / destination width
  float   scale_y            = 1.0F;  ///< crop height / destination height
};
static_assert(sizeof(CropResizePushConstants) == 40);

[[nodiscard]] inline auto CropResizeLinearDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kCropResizeLinear),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(CropResizePushConstants),
          {}};
}

}  // namespace alcedo::vulkan::geometry_kernels
