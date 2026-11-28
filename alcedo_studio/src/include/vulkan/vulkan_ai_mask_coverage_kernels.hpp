//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `ai_mask_coverage` (src/edit/runtime/vulkan/shader/ai_mask_coverage.slang): the
// Feather and Expand passes of AI Mask coverage (SM8 section 5.1) of the OpenCL program
// opencl_dag_ai_mask_coverage (ai_mask_coverage.cl), phase VK11.

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::ai_mask_coverage_kernels {

/// One 3 x 3 dilate or erode step on the R8 values.
inline constexpr std::string_view kMorphology     = "ai_mask_morphology_r8";
/// The horizontal Feather pass into the R32F rows image.
inline constexpr std::string_view kFeatherRows    = "ai_mask_feather_rows_r32f";
/// The vertical Feather pass over the rows into R8.
inline constexpr std::string_view kFeatherColumns = "ai_mask_feather_columns_r8";

/// Workgroup 16 x 16 x 1 of every kernel (the OpenCL local size).
inline constexpr uint32_t         kGroupSize      = 16;
/// The R8 input of the morphology and of the horizontal Feather pass.
inline constexpr uint32_t         kInputBinding   = 0;
/// The R8 output of the morphology and of the vertical Feather pass.
inline constexpr uint32_t         kOutputBinding  = 1;
/// The R32F rows image: written by the horizontal pass, read by the vertical pass.
inline constexpr uint32_t         kRowsBinding    = 2;

/// Push constants of the three kernels (scalar layout): `AiMaskEdgeGrid` of
/// ai_mask_coverage.cl, the morphology operation, and the Feather half kernel by device address.
struct EdgePushConstants {
  VkDeviceAddress weights       = 0;  ///< float[radius + 1]; 0 for the morphology
  int32_t         width         = 0;
  int32_t         height        = 0;
  int32_t         source_x      = 0;
  int32_t         source_y      = 0;
  int32_t         source_width  = 0;
  int32_t         source_height = 0;
  int32_t         dilate        = 1;
  int32_t         square        = 0;
  int32_t         radius        = 0;
  int32_t         pad           = 0;
};
static_assert(sizeof(EdgePushConstants) == 48);

template <class PushConstants>
[[nodiscard]] inline auto Descriptor(std::string_view                 kernel,
                                     std::vector<VulkanKernelBinding> bindings)
    -> VulkanKernelDescriptor {
  return {std::string(kernel), std::move(bindings), sizeof(PushConstants), {}};
}

[[nodiscard]] inline auto MorphologyDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<EdgePushConstants>(kMorphology,
                                       {{kInputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                        {kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto FeatherRowsDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<EdgePushConstants>(kFeatherRows,
                                       {{kInputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                        {kRowsBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

[[nodiscard]] inline auto FeatherColumnsDescriptor() -> VulkanKernelDescriptor {
  return Descriptor<EdgePushConstants>(kFeatherColumns,
                                       {{kOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                        {kRowsBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});
}

/// The descriptors of the three kernels of the group, for the kernel catalog.
[[nodiscard]] inline auto AllDescriptors() -> std::array<VulkanKernelDescriptor, 3> {
  return {MorphologyDescriptor(), FeatherRowsDescriptor(), FeatherColumnsDescriptor()};
}

}  // namespace alcedo::vulkan::ai_mask_coverage_kernels
