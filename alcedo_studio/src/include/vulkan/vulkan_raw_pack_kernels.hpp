//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `raw_pack`
// (src/decoders/processor/operators/gpu/vulkan_shader/raw_pack.slang): the crop, inverse white
// balance, and orientation of the demosaic result into the RGBA32F image of the graph, the forms
// of the OpenCL cvt_ref_space.cl pack kernels (phase VK4).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::raw_pack_kernels {

/// R, G, B planes to the destination image.
inline constexpr std::string_view kPackPlanes         = "pack_planes_crop_inverse_orient";
/// An RGBA buffer to the destination image.
inline constexpr std::string_view kCopyRgba           = "copy_rgba_crop_inverse_orient";
/// Workgroup 16 x 16 x 1 for both kernels.
inline constexpr uint32_t         kGroupSize          = 16;
/// The RGBA32F storage image that both kernels write (set 0).
inline constexpr uint32_t         kDestinationBinding = 0;

/// `PackOrientParams` of cvt_ref_space.cl (scalar layout).
struct PackOrientParams {
  uint32_t src_x        = 0;
  uint32_t src_y        = 0;
  uint32_t src_width    = 0;  ///< crop width
  uint32_t src_height   = 0;  ///< crop height
  uint32_t dst_width    = 0;
  uint32_t dst_height   = 0;
  uint32_t flip         = 0;
  float    scale_r      = 1.0F;
  float    scale_g      = 1.0F;
  float    scale_b      = 1.0F;
  uint32_t plane_stride = 0;  ///< values in a row of each plane
  uint32_t src_stride   = 0;  ///< texels in a row of the RGBA source
};
static_assert(sizeof(PackOrientParams) == 48);

/// Push constants of `pack_planes_crop_inverse_orient` (scalar layout).
struct PackPlanesPushConstants {
  VkDeviceAddress  r = 0;
  VkDeviceAddress  g = 0;
  VkDeviceAddress  b = 0;
  PackOrientParams params;
};
static_assert(sizeof(PackPlanesPushConstants) == 72);

/// Push constants of `copy_rgba_crop_inverse_orient` (scalar layout).
struct CopyRgbaPushConstants {
  VkDeviceAddress  source = 0;  ///< RGBA float values
  PackOrientParams params;
};
static_assert(sizeof(CopyRgbaPushConstants) == 56);

[[nodiscard]] inline auto PackPlanesDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kPackPlanes),
          {{kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(PackPlanesPushConstants),
          {}};
}

[[nodiscard]] inline auto CopyRgbaDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kCopyRgba),
          {{kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(CopyRgbaPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::raw_pack_kernels
