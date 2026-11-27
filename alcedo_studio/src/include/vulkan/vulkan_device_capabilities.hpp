//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo::vulkan {

/// One queue family of a physical device (`VkQueueFamilyProperties`).
struct VulkanQueueFamily {
  uint32_t     index                = 0;
  VkQueueFlags flags                = 0;
  uint32_t     queue_count          = 0;
  uint32_t     timestamp_valid_bits = 0;
};

/// The optimal-tiling features of one image format (`VkFormatProperties3`).
struct VulkanFormatSupport {
  VkFormat              format                  = VK_FORMAT_UNDEFINED;
  VkFormatFeatureFlags2 optimal_tiling_features = 0;
};

/// The device features that the pipeline reads. Each value is the `VkBool32` of the driver.
struct VulkanDeviceFeatures {
  bool buffer_device_address = false;  ///< Vulkan 1.2 `bufferDeviceAddress`
  bool scalar_block_layout   = false;  ///< Vulkan 1.2 `scalarBlockLayout`
  bool shader_int64          = false;  ///< Vulkan 1.0 `shaderInt64`
  bool synchronization2      = false;  ///< Vulkan 1.3 `synchronization2`
  bool shader_float16        = false;  ///< Vulkan 1.2 `shaderFloat16` (optional)
  /// `memoryPriority` of `VK_EXT_memory_priority` (optional; false without the extension)
  bool memory_priority       = false;
};

/**
 * @brief The properties of one physical device that device selection and the requirement check
 * use.
 *
 * `ReadVulkanDeviceCapabilities` fills it from the driver one time for each device. Tests fill it
 * by hand to give recorded property sets to `CheckVulkanDeviceRequirements`. The value has no
 * Vulkan handle and no owner: it is a description of the device, and `VulkanContext` keeps the
 * one of the selected device for its lifetime.
 */
struct VulkanDeviceCapabilities {
  std::string                                      name;
  uint32_t                                         vendor_id      = 0;
  uint32_t                                         device_id      = 0;
  VkPhysicalDeviceType                             device_type    = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  uint32_t                                         api_version    = 0;
  uint32_t                                         driver_version = 0;
  /// `pipelineCacheUUID`: pipeline cache data of another value is from another driver.
  std::array<uint8_t, VK_UUID_SIZE>                pipeline_cache_uuid{};
  /// The LUID of the adapter (Windows). VK18 matches the display device to DXGI with it.
  std::optional<std::array<uint8_t, VK_LUID_SIZE>> luid;
  /// Names of the device extensions, sorted.
  std::vector<std::string>                         extensions;
  VulkanDeviceFeatures                             features;
  std::vector<VulkanQueueFamily>                   queue_families;
  /// The formats of `PipelineImageFormats()`, in that order.
  std::vector<VulkanFormatSupport>                 formats;
  VkPhysicalDeviceLimits                           limits{};
  /// Size of the largest `DEVICE_LOCAL` memory heap.
  uint64_t                                         device_local_heap_bytes = 0;

  /// True when @ref extensions has @p extension_name.
  [[nodiscard]] auto HasExtension(std::string_view extension_name) const -> bool;
  /// The optimal-tiling features of @p format, or 0 when @ref formats has no entry for it.
  [[nodiscard]] auto FormatFeatures(VkFormat format) const -> VkFormatFeatureFlags2;
};

/// The three image formats of the pipeline (plan section 4.2): RGBA32F, R32F, and R8.
[[nodiscard]] auto PipelineImageFormats() -> std::span<const VkFormat>;

/// The name of a format of `PipelineImageFormats()`, for example `VK_FORMAT_R32_SFLOAT`.
[[nodiscard]] auto PipelineImageFormatName(VkFormat format) -> std::string_view;

/// `major.minor.patch` of a `VK_MAKE_API_VERSION` value.
[[nodiscard]] auto FormatVulkanApiVersion(uint32_t api_version) -> std::string;

/**
 * @brief Reads the capabilities of @p physical_device from the driver.
 *
 * Precondition: volk has loaded the instance functions of the instance that enumerated the
 * device (`VulkanContext` does this before it calls the function). No side effect.
 */
[[nodiscard]] auto ReadVulkanDeviceCapabilities(VkPhysicalDevice physical_device)
    -> VulkanDeviceCapabilities;

}  // namespace alcedo::vulkan
