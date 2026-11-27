//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `debayer_rcd`
// (src/decoders/processor/operators/gpu/vulkan_shader/debayer_rcd.slang): the five RCD demosaic
// kernels of the OpenCL debayer_rcd.cl (phase VK4). They run in the order of kRcdKernels, with a
// barrier between them.

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::debayer_rcd_kernels {

/// Split of the CFA into R, G, B planes, and the vertical and horizontal direction weight.
inline constexpr std::string_view                kInitAndVh  = "rcd_init_and_vh";
/// Green at the red and blue samples.
inline constexpr std::string_view                kGreenAtRb  = "rcd_green_at_rb";
/// The diagonal direction weight at the red and blue samples.
inline constexpr std::string_view                kPqDir      = "rcd_pq_dir";
/// Blue at the red samples and red at the blue samples.
inline constexpr std::string_view                kRbAtRb     = "rcd_rb_at_rb";
/// Red and blue at the green samples.
inline constexpr std::string_view                kRbAtG      = "rcd_rb_at_g";
/// The five kernels in the order of `OpenCL::EncodeBayerRcd`.
inline constexpr std::array<std::string_view, 5> kRcdKernels = {kInitAndVh, kGreenAtRb, kPqDir,
                                                                kRbAtRb, kRbAtG};
/// Workgroup 16 x 16 x 1 for the five kernels.
inline constexpr uint32_t                        kGroupSize  = 16;

/// `SinglePlaneParams` of debayer_rcd.cl (scalar layout).
struct SinglePlaneParams {
  uint32_t width     = 0;
  uint32_t height    = 0;
  uint32_t stride    = 0;  ///< values in a row of each plane
  uint32_t rgb_fc[4] = {};
};
static_assert(sizeof(SinglePlaneParams) == 28);

/// Push constants of the five kernels (scalar layout). Each kernel reads the planes of its
/// OpenCL arguments; each plane has width * height float values.
struct RcdPushConstants {
  VkDeviceAddress   raw = 0;  ///< the linear CFA
  VkDeviceAddress   r   = 0;
  VkDeviceAddress   g   = 0;
  VkDeviceAddress   b   = 0;
  VkDeviceAddress   vh  = 0;  ///< vertical and horizontal weight
  VkDeviceAddress   pq  = 0;  ///< diagonal weight
  SinglePlaneParams params;
  uint32_t          reserved = 0;  ///< pads the block to the 8-byte struct alignment
};
static_assert(sizeof(RcdPushConstants) == 80);

[[nodiscard]] inline auto Descriptor(std::string_view kernel) -> VulkanKernelDescriptor {
  return {std::string(kernel), {}, sizeof(RcdPushConstants), {}};
}

}  // namespace alcedo::vulkan::debayer_rcd_kernels
