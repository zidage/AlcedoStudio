//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_requirements.hpp"

#include <string>

namespace alcedo::vulkan {

auto VulkanRequirementResult::Describe() const -> std::string {
  std::string text;
  for (const auto& requirement : absent_requirements) {
    if (!text.empty()) {
      text += ", ";
    }
    text += requirement;
  }
  return text;
}

auto FindPipelineQueueFamily(const VulkanDeviceCapabilities& capabilities)
    -> std::optional<uint32_t> {
  constexpr VkQueueFlags kRequiredFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  for (const auto& family : capabilities.queue_families) {
    if ((family.flags & kRequiredFlags) == kRequiredFlags && family.queue_count > 0 &&
        family.timestamp_valid_bits > 0) {
      return family.index;
    }
  }
  return std::nullopt;
}

auto CheckVulkanDeviceRequirements(const VulkanDeviceCapabilities& capabilities)
    -> VulkanRequirementResult {
  VulkanRequirementResult result;
  auto&                   absent = result.absent_requirements;

  if (capabilities.api_version < kRequiredVulkanApiVersion) {
    absent.push_back("Vulkan " + FormatVulkanApiVersion(kRequiredVulkanApiVersion) +
                     " (the device has Vulkan " + FormatVulkanApiVersion(capabilities.api_version) +
                     ")");
  }
  for (const char* extension : kRequiredDeviceExtensions) {
    if (!capabilities.HasExtension(extension)) {
      absent.emplace_back(extension);
    }
  }

  const auto& features = capabilities.features;
  if (!features.buffer_device_address) {
    absent.emplace_back("bufferDeviceAddress");
  }
  if (!features.scalar_block_layout) {
    absent.emplace_back("scalarBlockLayout");
  }
  if (!features.shader_int64) {
    absent.emplace_back("shaderInt64");
  }
  if (!features.synchronization2) {
    absent.emplace_back("synchronization2");
  }

  if (!FindPipelineQueueFamily(capabilities).has_value()) {
    absent.emplace_back("a queue family with graphics, compute, and timestamps");
  }

  for (const VkFormat format : PipelineImageFormats()) {
    const VkFormatFeatureFlags2 features_of_format = capabilities.FormatFeatures(format);
    const std::string           format_name{PipelineImageFormatName(format)};
    if ((features_of_format & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) == 0) {
      absent.push_back(format_name + " storage image");
    }
    if ((features_of_format & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0) {
      absent.push_back(format_name + " sampled image");
    }
    if ((features_of_format & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
      absent.push_back(format_name + " linear filtering");
    }
  }
  return result;
}

}  // namespace alcedo::vulkan
