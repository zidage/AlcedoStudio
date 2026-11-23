//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_device_selection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/vulkan_requirements.hpp"

namespace alcedo::vulkan {
namespace {

auto ToLower(std::string_view text) -> std::string {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

auto NameMatches(std::string_view name, std::string_view lower_filter) -> bool {
  return lower_filter.empty() || ToLower(name).find(lower_filter) != std::string::npos;
}

auto JoinDeviceNames(std::span<const VulkanDeviceCapabilities> devices) -> std::string {
  std::string names;
  for (const auto& device : devices) {
    if (!names.empty()) {
      names += ", ";
    }
    names += device.name;
  }
  return names.empty() ? std::string("none") : names;
}

}  // namespace

auto SelectVulkanDevice(std::span<const VulkanDeviceCapabilities> devices,
                        std::string_view                          name_filter) -> std::size_t {
  const std::string        lower_filter = ToLower(name_filter);

  std::vector<std::size_t> passing;
  std::string              rejected;
  bool                     any_name_match = false;
  for (std::size_t index = 0; index < devices.size(); ++index) {
    const auto& device = devices[index];
    if (!NameMatches(device.name, lower_filter)) {
      continue;
    }
    any_name_match    = true;
    const auto result = CheckVulkanDeviceRequirements(device);
    if (result.Passed()) {
      passing.push_back(index);
      continue;
    }
    if (!rejected.empty()) {
      rejected += "; ";
    }
    rejected += device.name + " does not have: " + result.Describe();
  }

  if (!any_name_match) {
    if (lower_filter.empty()) {
      throw std::runtime_error("Vulkan: the instance has no physical device.");
    }
    throw std::runtime_error("Vulkan: ALCEDO_VULKAN_DEVICE '" + std::string(name_filter) +
                             "' matches no device. Devices: " + JoinDeviceNames(devices) + ".");
  }
  if (passing.empty()) {
    throw std::runtime_error("Vulkan: no device has the requirements of the Vulkan backend. " +
                             rejected + ".");
  }

  constexpr std::array<VkPhysicalDeviceType, 2> kPreferredTypes = {
      VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
      VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
  };
  for (const VkPhysicalDeviceType type : kPreferredTypes) {
    for (const std::size_t index : passing) {
      if (devices[index].device_type == type) {
        return index;
      }
    }
  }
  return passing.front();
}

}  // namespace alcedo::vulkan
