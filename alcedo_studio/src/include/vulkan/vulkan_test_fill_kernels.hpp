//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `test_fill` (src/vulkan/shader/test_fill.slang): the check kernels of the
// Vulkan backend. They write a known value to a buffer or to an RGBA32F storage image, so a test
// or `alcedo-gpu-check --vulkan` can prove that a pipeline from the cache runs.

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::test_fill_kernels {

/// output[i] = value + addend for i < count. Workgroup 64 x 1 x 1.
inline constexpr std::string_view kFillBuffer          = "test_fill";
/// Each texel of the image at `kImageBinding` = value. Workgroup 8 x 8 x 1.
inline constexpr std::string_view kFillImage           = "test_fill_image";

inline constexpr uint32_t         kFillBufferGroupSize = 64;
inline constexpr uint32_t         kFillImageGroupSize  = 8;
/// The RGBA32F storage image of `test_fill_image` (set 0).
inline constexpr uint32_t         kImageBinding        = 0;
/// The specialization constant that `test_fill` adds to each value (default 0).
inline constexpr uint32_t         kAddendConstantId    = 0;

/// Push constants of `test_fill` (scalar layout).
struct FillBufferPushConstants {
  VkDeviceAddress output = 0;  ///< device address of `count` uint32 values
  uint32_t        count  = 0;
  uint32_t        value  = 0;
};
static_assert(sizeof(FillBufferPushConstants) == 16);

/// Push constants of `test_fill_image` (scalar layout).
struct FillImagePushConstants {
  float    value[4] = {};
  uint32_t width    = 0;
  uint32_t height   = 0;
};
static_assert(sizeof(FillImagePushConstants) == 24);

/// The descriptor of `test_fill`. @p specialization can set `kAddendConstantId`; each set of
/// values is a pipeline of its own.
[[nodiscard]] inline auto FillBufferDescriptor(
    std::vector<VulkanSpecializationValue> specialization = {}) -> VulkanKernelDescriptor {
  return {std::string(kFillBuffer), {}, sizeof(FillBufferPushConstants), std::move(specialization)};
}

/// The descriptor of `test_fill_image`.
[[nodiscard]] inline auto FillImageDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kFillImage),
          {{kImageBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}},
          sizeof(FillImagePushConstants),
          {}};
}

}  // namespace alcedo::vulkan::test_fill_kernels
