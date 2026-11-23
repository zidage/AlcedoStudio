//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_device_capabilities.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace alcedo::vulkan {
namespace {

constexpr std::array<VkFormat, 3> kPipelineImageFormats = {
    VK_FORMAT_R32G32B32A32_SFLOAT,
    VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R8_UNORM,
};

auto ReadExtensionNames(VkPhysicalDevice physical_device) -> std::vector<std::string> {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> properties(count);
  vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, properties.data());
  properties.resize(count);

  std::vector<std::string> names;
  names.reserve(properties.size());
  for (const auto& extension : properties) {
    names.emplace_back(extension.extensionName);
  }
  std::sort(names.begin(), names.end());
  return names;
}

auto ReadFeatures(VkPhysicalDevice physical_device, uint32_t api_version,
                  bool has_memory_priority_extension) -> VulkanDeviceFeatures {
  // A feature structure of a Vulkan version or an extension that the device does not have is not
  // valid in the chain.
  VkPhysicalDeviceMemoryPriorityFeaturesEXT memory_priority{};
  memory_priority.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

  const bool has_12 = api_version >= VK_API_VERSION_1_2;
  const bool has_13 = api_version >= VK_API_VERSION_1_3;
  if (has_12) {
    features.pNext = &features12;
  }
  if (has_13) {
    features12.pNext = &features13;
  }
  if (has_memory_priority_extension) {
    memory_priority.pNext = features.pNext;
    features.pNext        = &memory_priority;
  }
  vkGetPhysicalDeviceFeatures2(physical_device, &features);

  VulkanDeviceFeatures result;
  result.shader_int64 = features.features.shaderInt64 == VK_TRUE;
  result.memory_priority =
      has_memory_priority_extension && memory_priority.memoryPriority == VK_TRUE;
  if (has_12) {
    result.buffer_device_address = features12.bufferDeviceAddress == VK_TRUE;
    result.scalar_block_layout   = features12.scalarBlockLayout == VK_TRUE;
    result.shader_float16        = features12.shaderFloat16 == VK_TRUE;
  }
  if (has_13) {
    result.synchronization2 = features13.synchronization2 == VK_TRUE;
  }
  return result;
}

auto ReadQueueFamilies(VkPhysicalDevice physical_device) -> std::vector<VulkanQueueFamily> {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, properties.data());
  properties.resize(count);

  std::vector<VulkanQueueFamily> families;
  families.reserve(properties.size());
  for (uint32_t index = 0; index < properties.size(); ++index) {
    const auto& family = properties[index];
    families.push_back(VulkanQueueFamily{
        .index                = index,
        .flags                = family.queueFlags,
        .queue_count          = family.queueCount,
        .timestamp_valid_bits = family.timestampValidBits,
    });
  }
  return families;
}

auto ReadFormatSupport(VkPhysicalDevice physical_device, uint32_t api_version)
    -> std::vector<VulkanFormatSupport> {
  std::vector<VulkanFormatSupport> formats;
  formats.reserve(kPipelineImageFormats.size());
  for (const VkFormat format : kPipelineImageFormats) {
    VkFormatProperties3 properties3{};
    properties3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
    VkFormatProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    // VkFormatProperties3 is core in Vulkan 1.3. Below 1.3 the 32-bit flags have the same bit
    // positions for the storage, sampled, and linear filter features.
    const bool has_13 = api_version >= VK_API_VERSION_1_3;
    if (has_13) {
      properties2.pNext = &properties3;
    }
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties2);
    formats.push_back(VulkanFormatSupport{
        .format                  = format,
        .optimal_tiling_features = has_13 ? properties3.optimalTilingFeatures
                                          : static_cast<VkFormatFeatureFlags2>(
                                                properties2.formatProperties.optimalTilingFeatures),
    });
  }
  return formats;
}

auto ReadLargestDeviceLocalHeap(VkPhysicalDevice physical_device) -> uint64_t {
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
  uint64_t largest = 0;
  for (uint32_t index = 0; index < memory.memoryHeapCount; ++index) {
    const auto& heap = memory.memoryHeaps[index];
    if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
      largest = std::max<uint64_t>(largest, heap.size);
    }
  }
  return largest;
}

}  // namespace

auto VulkanDeviceCapabilities::HasExtension(std::string_view extension_name) const -> bool {
  return std::binary_search(
      extensions.begin(), extensions.end(), extension_name,
      [](std::string_view left, std::string_view right) { return left < right; });
}

auto VulkanDeviceCapabilities::FormatFeatures(VkFormat format) const -> VkFormatFeatureFlags2 {
  for (const auto& support : formats) {
    if (support.format == format) {
      return support.optimal_tiling_features;
    }
  }
  return 0;
}

auto PipelineImageFormats() -> std::span<const VkFormat> { return kPipelineImageFormats; }

auto PipelineImageFormatName(VkFormat format) -> std::string_view {
  switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT:
      return "VK_FORMAT_R32G32B32A32_SFLOAT";
    case VK_FORMAT_R32_SFLOAT:
      return "VK_FORMAT_R32_SFLOAT";
    case VK_FORMAT_R8_UNORM:
      return "VK_FORMAT_R8_UNORM";
    default:
      return "VkFormat (not a pipeline image format)";
  }
}

auto FormatVulkanApiVersion(uint32_t api_version) -> std::string {
  return std::to_string(VK_API_VERSION_MAJOR(api_version)) + "." +
         std::to_string(VK_API_VERSION_MINOR(api_version)) + "." +
         std::to_string(VK_API_VERSION_PATCH(api_version));
}

auto ReadVulkanDeviceCapabilities(VkPhysicalDevice physical_device) -> VulkanDeviceCapabilities {
  VkPhysicalDeviceIDProperties id_properties{};
  id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &id_properties;
  vkGetPhysicalDeviceProperties2(physical_device, &properties);
  const auto&              core = properties.properties;

  VulkanDeviceCapabilities capabilities;
  capabilities.name           = core.deviceName;
  capabilities.vendor_id      = core.vendorID;
  capabilities.device_id      = core.deviceID;
  capabilities.device_type    = core.deviceType;
  capabilities.api_version    = core.apiVersion;
  capabilities.driver_version = core.driverVersion;
  std::copy(std::begin(core.pipelineCacheUUID), std::end(core.pipelineCacheUUID),
            capabilities.pipeline_cache_uuid.begin());
  if (id_properties.deviceLUIDValid == VK_TRUE) {
    std::array<uint8_t, VK_LUID_SIZE> luid{};
    std::copy(std::begin(id_properties.deviceLUID), std::end(id_properties.deviceLUID),
              luid.begin());
    capabilities.luid = luid;
  }
  capabilities.extensions = ReadExtensionNames(physical_device);
  capabilities.features =
      ReadFeatures(physical_device, core.apiVersion,
                   capabilities.HasExtension(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME));
  capabilities.queue_families          = ReadQueueFamilies(physical_device);
  capabilities.formats                 = ReadFormatSupport(physical_device, core.apiVersion);
  capabilities.limits                  = core.limits;
  capabilities.device_local_heap_bytes = ReadLargestDeviceLocalHeap(physical_device);
  return capabilities;
}

}  // namespace alcedo::vulkan
