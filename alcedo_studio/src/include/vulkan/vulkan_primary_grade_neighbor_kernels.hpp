//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `primary_grade_neighbor`
// (src/edit/runtime/vulkan/shader/primary_grade_neighbor.slang): the two separable neighborhood
// kernels of the OpenCL program opencl_dag_primary_grade (primary_grade_neighbor.cl) that the
// product dispatches (phase VK9). The DRT finish runs Clarity, Sharpen, and Halation with them.

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::primary_grade_neighbor_kernels {

/// The horizontal blur of a scene image into the RGBA32F scratch image.
inline constexpr std::string_view kBlurHorizontal = "primary_grade_neighbor_blur_h_scene_rgba32f";
/// The vertical blur of the scratch image, the adjustment, and the optional (masked) mix.
inline constexpr std::string_view kApplyVertical  = "primary_grade_neighbor_apply_v_scene_rgba32f";
/// Workgroup 8 x 8 x 1 of both kernels (the OpenCL local size).
inline constexpr uint32_t         kGroupSize      = 8;
/// The largest vertical radius: the workgroup tile of the vertical kernel holds the color of
/// kGroupSize x (kGroupSize + 2 x kMaxRadius) texels (12 864 bytes, inside the Vulkan minimum of
/// 16 KiB of workgroup memory).
inline constexpr uint32_t         kMaxRadius      = 63;
/// The RGBA32F storage image of a cached source image (set 0); a dummy when the source is a
/// scene-work buffer.
inline constexpr uint32_t         kSourceBinding  = 0;
/// The RGBA32F scratch image: written by the horizontal kernel, read by the vertical kernel.
inline constexpr uint32_t         kBlurBinding    = 1;
/// The RGBA32F destination image of the vertical kernel; a dummy for a scene-work destination.
inline constexpr uint32_t         kDestinationBinding = 2;
/// The RGBA32F storage image of a cached mix source; a dummy otherwise.
inline constexpr uint32_t         kMixSourceBinding   = 3;
/// The R8 storage image of the mask; a dummy without a mask.
inline constexpr uint32_t         kMaskBinding        = 4;

/// Push constants of `primary_grade_neighbor_blur_h_scene_rgba32f` (scalar layout).
struct BlurPushConstants {
  VkDeviceAddress params = 0;  ///< GradeNeighborParams (300 bytes)
  VkDeviceAddress source = 0;  ///< RGBA32F row-major, or 0: the source image binding
  uint32_t        width  = 0;
  uint32_t        height = 0;
};
static_assert(sizeof(BlurPushConstants) == 24);

/// Push constants of `primary_grade_neighbor_apply_v_scene_rgba32f` (scalar layout).
struct ApplyPushConstants {
  VkDeviceAddress params      = 0;  ///< GradeNeighborParams (300 bytes)
  VkDeviceAddress source      = 0;  ///< RGBA32F row-major, or 0: the source image binding
  VkDeviceAddress destination = 0;  ///< RGBA32F row-major, or 0: the destination image binding
  VkDeviceAddress mix_source  = 0;  ///< RGBA32F row-major, or 0: the mix image binding
  uint32_t        width       = 0;
  uint32_t        height      = 0;
  uint32_t        apply_mix   = 0;
  uint32_t        has_mask    = 0;
  float           grade_mix   = 1.0F;
  uint32_t        pad[3]      = {};
};
static_assert(sizeof(ApplyPushConstants) == 64);

[[nodiscard]] inline auto BlurDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kBlurHorizontal),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kBlurBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(BlurPushConstants),
          {}};
}

[[nodiscard]] inline auto ApplyDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kApplyVertical),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kBlurBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kDestinationBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kMixSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kMaskBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(ApplyPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::primary_grade_neighbor_kernels
