//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vulkan/vulkan_device_capabilities.hpp"

namespace alcedo::vulkan {

/// The lowest Vulkan version that the backend accepts (plan decision B4).
inline constexpr uint32_t    kRequiredVulkanApiVersion   = VK_API_VERSION_1_3;

/// The device extensions that the backend requires (plan section 4.2 and phase VK0).
inline constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
};

/// The result of `CheckVulkanDeviceRequirements`.
struct VulkanRequirementResult {
  /// One entry for each absent requirement, in the order of the check. Each entry names the
  /// requirement as the Vulkan specification spells it (`VK_KHR_push_descriptor`,
  /// `bufferDeviceAddress`), and the API version entry includes the version of the device.
  std::vector<std::string> absent_requirements;

  [[nodiscard]] auto       Passed() const -> bool { return absent_requirements.empty(); }
  /// The absent requirements separated by `, `. Empty when @ref Passed is true.
  [[nodiscard]] auto       Describe() const -> std::string;
};

/**
 * @brief Finds the queue family of the pipeline queue: the first family with graphics, compute,
 * and timestamps (`timestampValidBits` above 0).
 *
 * The pipeline device and the display device each use queue 0 of this family (plan decision B5),
 * so a device with one queue in the family is sufficient.
 */
[[nodiscard]] auto FindPipelineQueueFamily(const VulkanDeviceCapabilities& capabilities)
    -> std::optional<uint32_t>;

/**
 * @brief Checks @p capabilities against the requirements of the Vulkan backend.
 *
 * Requirements: Vulkan 1.3, `VK_KHR_push_descriptor`, `VK_EXT_memory_budget`,
 * `bufferDeviceAddress`, `scalarBlockLayout`, `shaderInt64`, `synchronization2`, a pipeline
 * queue family (`FindPipelineQueueFamily`), and for each format of `PipelineImageFormats()` with
 * optimal tiling: storage image, sampled image, and linear filtering. Pure function; it reports
 * every absent requirement, not only the first.
 */
[[nodiscard]] auto CheckVulkanDeviceRequirements(const VulkanDeviceCapabilities& capabilities)
    -> VulkanRequirementResult;

}  // namespace alcedo::vulkan
