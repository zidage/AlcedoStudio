//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `primary_grade` (src/edit/runtime/vulkan/shader/primary_grade.slang): the point
// kernels of the Color Grade of the OpenCL program opencl_dag_primary_grade that the Grade pass
// calls (phase VK8).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::primary_grade_kernels {

/// The adjustments of one Grade stage and the optional (masked) mix with the Grade input.
inline constexpr std::string_view kPointwiseScene      = "primary_grade_pointwise_scene_rgba32f";
/// The histogram of the curve input (issue #888).
inline constexpr std::string_view kCurveInputHistogram = "primary_grade_curve_input_histogram";
/// Workgroup 16 x 16 x 1 of the point kernel.
inline constexpr uint32_t         kGroupSize           = 16;
/// Workgroup 256 x 1 x 1 of the histogram kernel, and its largest workgroup count (the OpenCL
/// dispatch).
inline constexpr uint32_t         kHistogramGroupSize  = 256;
inline constexpr uint32_t         kHistogramMaxGroups  = 128;
/// The RGBA32F storage image of a cached source image (set 0); a dummy when the source is a
/// scene-work buffer.
inline constexpr uint32_t         kSourceBinding       = 0;
/// The RGBA32F storage image of a cached mix source (set 0); a dummy otherwise.
inline constexpr uint32_t         kMixSourceBinding    = 1;
/// The R8 storage image of the Grade mask (set 0); a dummy without a mask.
inline constexpr uint32_t         kMaskBinding         = 2;

/// Push constants of `primary_grade_pointwise_scene_rgba32f` (scalar layout).
struct PointwisePushConstants {
  VkDeviceAddress parameter_base = 0;  ///< the parameter arena buffer
  VkDeviceAddress commands       = 0;  ///< uint32 record offsets, in apply order
  VkDeviceAddress lut            = 0;  ///< RGBA32F voxels, X fastest (a dummy for an edge < 2)
  VkDeviceAddress source         = 0;  ///< RGBA32F row-major, or 0: the source image binding
  VkDeviceAddress destination    = 0;  ///< RGBA32F row-major (a scene-work member)
  VkDeviceAddress mix_source     = 0;  ///< RGBA32F row-major, or 0: the mix image binding
  uint32_t        command_offset = 0;
  uint32_t        command_count  = 0;
  uint32_t        lut_edge       = 0;
  uint32_t        width          = 0;
  uint32_t        height         = 0;
  uint32_t        apply_mix      = 0;
  uint32_t        has_mask       = 0;
  float           grade_mix      = 1.0F;
};
static_assert(sizeof(PointwisePushConstants) == 80);

/// Push constants of `primary_grade_curve_input_histogram` (scalar layout).
struct CurveInputHistogramPushConstants {
  VkDeviceAddress parameter_base   = 0;
  VkDeviceAddress commands         = 0;  ///< the records before the Curve
  VkDeviceAddress source           = 0;  ///< RGBA32F row-major, or 0: the source image binding
  VkDeviceAddress counts           = 0;  ///< 768 uint32: red, green, blue bins
  uint32_t        width            = 0;
  uint32_t        step             = 0;
  uint32_t        sampled_width    = 0;
  uint32_t        sample_count     = 0;
  uint32_t        command_count    = 0;
  uint32_t        invocation_count = 0;  ///< workgroups * kHistogramGroupSize
};
static_assert(sizeof(CurveInputHistogramPushConstants) == 56);

[[nodiscard]] inline auto PointwiseSceneDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kPointwiseScene),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kMixSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
           {kMaskBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(PointwisePushConstants),
          {}};
}

[[nodiscard]] inline auto CurveInputHistogramDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kCurveInputHistogram),
          {{kSourceBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(CurveInputHistogramPushConstants),
          {}};
}

}  // namespace alcedo::vulkan::primary_grade_kernels
