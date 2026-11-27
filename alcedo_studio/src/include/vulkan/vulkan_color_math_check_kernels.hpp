//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The kernel group `color_math_check` (src/edit/runtime/vulkan/shader/color_math_check.slang):
// check kernels that evaluate the shared color math of the Vulkan kernels for a list of inputs, so
// a test can compare each value with the OpenCL program of the same functions (phase VK3).

#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "vulkan/vulkan_compute_pipeline_cache.hpp"

namespace alcedo::vulkan::color_math_check_kernels {

/// output[tf * count + i] = CeDecode or CeEncode of input[i] for each transfer function id tf.
inline constexpr std::string_view kTransfer  = "color_math_check_transfer";
/// output[i].rgb = one OkLab or ACEScc function of input[i].rgb; output[i].a = 0.
inline constexpr std::string_view kColor     = "color_math_check_color";
/// Workgroup 64 x 1 x 1 for both kernels.
inline constexpr uint32_t         kGroupSize = 64;

/// The function of `color_math_check_color`.
enum class ColorFunction : uint32_t {
  AcesccToLinearAp1 = 0,
  LinearAp1ToAcescc = 1,
  LinearAp1ToOkLab  = 2,
  OkLabToLinearAp1  = 3,
};

/// Push constants of `color_math_check_transfer` (scalar layout).
struct TransferPushConstants {
  VkDeviceAddress input  = 0;  ///< `count` float values
  VkDeviceAddress output = 0;  ///< `count` * CE_TF_COUNT float values
  uint32_t        count  = 0;
  uint32_t        encode = 0;  ///< 0: decode, 1: encode
};
static_assert(sizeof(TransferPushConstants) == 24);

/// Push constants of `color_math_check_color` (scalar layout).
struct ColorPushConstants {
  VkDeviceAddress input    = 0;  ///< `count` RGBA float values
  VkDeviceAddress output   = 0;  ///< `count` RGBA float values
  uint32_t        count    = 0;
  ColorFunction   function = ColorFunction::AcesccToLinearAp1;
};
static_assert(sizeof(ColorPushConstants) == 24);

[[nodiscard]] inline auto TransferDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kTransfer), {}, sizeof(TransferPushConstants), {}};
}

[[nodiscard]] inline auto ColorDescriptor() -> VulkanKernelDescriptor {
  return {std::string(kColor), {}, sizeof(ColorPushConstants), {}};
}

}  // namespace alcedo::vulkan::color_math_check_kernels
