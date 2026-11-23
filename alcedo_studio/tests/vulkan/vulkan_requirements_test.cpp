//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Requirement check and device selection of the Vulkan backend on recorded property sets. No GPU.

#include "vulkan/vulkan_requirements.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vulkan/vulkan_device_selection.hpp"

namespace alcedo::vulkan {
namespace {

constexpr VkFormatFeatureFlags2 kAllPipelineFormatFeatures =
    VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
    VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

// A device with every requirement. The queue families follow the vulkaninfo record of the
// development computer (plan section 3.5): the RTX 3080 Laptop GPU has 16 queues in its graphics
// family, Iris Xe has one.
auto DeviceWithAllRequirements(std::string name, VkPhysicalDeviceType type,
                               uint32_t graphics_queue_count) -> VulkanDeviceCapabilities {
  VulkanDeviceCapabilities device;
  device.name        = std::move(name);
  device.device_type = type;
  device.api_version = VK_MAKE_API_VERSION(0, 1, 4, 323);
  device.extensions  = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME,
                        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  std::sort(device.extensions.begin(), device.extensions.end());
  device.features.buffer_device_address = true;
  device.features.scalar_block_layout   = true;
  device.features.shader_int64          = true;
  device.features.synchronization2      = true;
  device.queue_families                 = {
      VulkanQueueFamily{
                          .index       = 0,
                          .flags       = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
                          .queue_count = graphics_queue_count,
                          .timestamp_valid_bits = 64},
      VulkanQueueFamily{.index                = 1,
                                        .flags                = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
                                        .queue_count          = 8,
                                        .timestamp_valid_bits = 64},
  };
  for (const VkFormat format : PipelineImageFormats()) {
    device.formats.push_back(VulkanFormatSupport{
        .format = format, .optimal_tiling_features = kAllPipelineFormatFeatures});
  }
  return device;
}

void RemoveExtension(VulkanDeviceCapabilities& device, std::string_view extension) {
  device.extensions.erase(
      std::remove(device.extensions.begin(), device.extensions.end(), extension),
      device.extensions.end());
}

void RemoveFormatFeature(VulkanDeviceCapabilities& device, VkFormat format,
                         VkFormatFeatureFlags2 feature) {
  for (auto& support : device.formats) {
    if (support.format == format) {
      support.optimal_tiling_features &= ~feature;
    }
  }
}

auto ThrownMessage(const std::vector<VulkanDeviceCapabilities>& devices, std::string_view filter)
    -> std::string {
  try {
    (void)SelectVulkanDevice(devices, filter);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

TEST(VulkanRequirementsTest, DeviceWithAllRequirementsPasses) {
  const auto result = CheckVulkanDeviceRequirements(
      DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16));
  EXPECT_TRUE(result.Passed()) << result.Describe();
  EXPECT_TRUE(result.Describe().empty());
}

TEST(VulkanRequirementsTest, DeviceWithoutPushDescriptorReportsTheExtensionName) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  RemoveExtension(device, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

  const auto result = CheckVulkanDeviceRequirements(device);

  ASSERT_FALSE(result.Passed());
  EXPECT_EQ(result.absent_requirements, std::vector<std::string>{"VK_KHR_push_descriptor"});
  EXPECT_EQ(result.Describe(), "VK_KHR_push_descriptor");
}

TEST(VulkanRequirementsTest, Vulkan12DeviceReportsTheApiVersion) {
  auto device        = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  device.api_version = VK_MAKE_API_VERSION(0, 1, 2, 198);
  // A Vulkan 1.2 device has no Vulkan 1.3 feature structure, so synchronization2 reads false.
  device.features.synchronization2 = false;

  const auto result                = CheckVulkanDeviceRequirements(device);

  ASSERT_EQ(result.absent_requirements.size(), 2U) << result.Describe();
  EXPECT_EQ(result.absent_requirements[0], "Vulkan 1.3.0 (the device has Vulkan 1.2.198)");
  EXPECT_EQ(result.absent_requirements[1], "synchronization2");
}

TEST(VulkanRequirementsTest, DeviceWithoutMemoryBudgetAndScalarLayoutReportsBoth) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  RemoveExtension(device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  device.features.scalar_block_layout = false;

  const auto result                   = CheckVulkanDeviceRequirements(device);

  EXPECT_EQ(result.absent_requirements,
            (std::vector<std::string>{"VK_EXT_memory_budget", "scalarBlockLayout"}));
  EXPECT_EQ(result.Describe(), "VK_EXT_memory_budget, scalarBlockLayout");
}

TEST(VulkanRequirementsTest, FormatWithoutLinearFilteringReportsTheFormatAndTheFeature) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  RemoveFormatFeature(device, VK_FORMAT_R32_SFLOAT,
                      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);

  const auto result = CheckVulkanDeviceRequirements(device);

  EXPECT_EQ(result.absent_requirements,
            std::vector<std::string>{"VK_FORMAT_R32_SFLOAT linear filtering"});
}

TEST(VulkanRequirementsTest, AbsentFormatRecordReportsEachFeatureOfTheFormat) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  device.formats.erase(device.formats.begin());  // VK_FORMAT_R32G32B32A32_SFLOAT

  const auto result = CheckVulkanDeviceRequirements(device);

  EXPECT_EQ(result.absent_requirements,
            (std::vector<std::string>{"VK_FORMAT_R32G32B32A32_SFLOAT storage image",
                                      "VK_FORMAT_R32G32B32A32_SFLOAT sampled image",
                                      "VK_FORMAT_R32G32B32A32_SFLOAT linear filtering"}));
}

TEST(VulkanRequirementsTest, GraphicsFamilyWithoutTimestampsReportsTheQueueRequirement) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  device.queue_families[0].timestamp_valid_bits = 0;

  const auto result                             = CheckVulkanDeviceRequirements(device);

  EXPECT_FALSE(FindPipelineQueueFamily(device).has_value());
  EXPECT_EQ(result.absent_requirements,
            std::vector<std::string>{"a queue family with graphics, compute, and timestamps"});
}

TEST(VulkanRequirementsTest, ComputeOnlyFamilyIsNotThePipelineQueueFamily) {
  auto device = DeviceWithAllRequirements("GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  device.queue_families[0].flags = VK_QUEUE_TRANSFER_BIT;

  // Family 1 has compute and timestamps but no graphics: the display device of plan decision B5
  // needs the same family as the pipeline device, so a compute-only family does not qualify.
  EXPECT_FALSE(FindPipelineQueueFamily(device).has_value());
}

TEST(VulkanRequirementsTest, SingleQueueGraphicsFamilyIsThePipelineQueueFamily) {
  const auto device = DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1);

  EXPECT_EQ(FindPipelineQueueFamily(device), std::optional<uint32_t>(0));
  EXPECT_TRUE(CheckVulkanDeviceRequirements(device).Passed());
}

TEST(VulkanDeviceSelectionTest, DefaultSelectionPrefersTheDiscreteDevice) {
  const std::vector<VulkanDeviceCapabilities> devices = {
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
      DeviceWithAllRequirements("NVIDIA GeForce RTX 3080 Laptop GPU",
                                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16),
  };

  EXPECT_EQ(SelectVulkanDevice(devices, ""), 1U);
}

TEST(VulkanDeviceSelectionTest, DefaultSelectionTakesTheIntegratedDeviceBeforeACpuDevice) {
  const std::vector<VulkanDeviceCapabilities> devices = {
      DeviceWithAllRequirements("llvmpipe", VK_PHYSICAL_DEVICE_TYPE_CPU, 1),
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
  };

  EXPECT_EQ(SelectVulkanDevice(devices, ""), 1U);
}

TEST(VulkanDeviceSelectionTest, NameFilterSelectsTheMatchingDeviceWithAnyLetterCase) {
  const std::vector<VulkanDeviceCapabilities> devices = {
      DeviceWithAllRequirements("NVIDIA GeForce RTX 3080 Laptop GPU",
                                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16),
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
  };

  EXPECT_EQ(SelectVulkanDevice(devices, "iris"), 1U);
  EXPECT_EQ(SelectVulkanDevice(devices, "RTX 3080"), 0U);
}

TEST(VulkanDeviceSelectionTest, NameFilterWithNoMatchReportsTheFilterAndTheDeviceNames) {
  const std::vector<VulkanDeviceCapabilities> devices = {
      DeviceWithAllRequirements("NVIDIA GeForce RTX 3080 Laptop GPU",
                                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16),
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
  };

  EXPECT_EQ(ThrownMessage(devices, "radeon"),
            "Vulkan: ALCEDO_VULKAN_DEVICE 'radeon' matches no device. Devices: NVIDIA GeForce "
            "RTX 3080 Laptop GPU, Intel(R) Iris(R) Xe Graphics.");
}

TEST(VulkanDeviceSelectionTest, MatchingDeviceWithoutARequirementReportsItsNameAndTheRequirement) {
  auto old_gpu = DeviceWithAllRequirements("Old GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  RemoveExtension(old_gpu, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  const std::vector<VulkanDeviceCapabilities> devices = {
      old_gpu,
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
  };

  // The filter excludes the device that passes, so the selection fails with the reason of the
  // device that the filter accepts. No other device is substituted.
  EXPECT_EQ(ThrownMessage(devices, "old"),
            "Vulkan: no device has the requirements of the Vulkan backend. Old GPU does not "
            "have: VK_KHR_push_descriptor.");
}

TEST(VulkanDeviceSelectionTest, DeviceWithoutTheRequirementsIsSkippedByTheDefaultRule) {
  auto old_gpu = DeviceWithAllRequirements("Old GPU", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 16);
  old_gpu.features.buffer_device_address              = false;
  const std::vector<VulkanDeviceCapabilities> devices = {
      old_gpu,
      DeviceWithAllRequirements("Intel(R) Iris(R) Xe Graphics",
                                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1),
  };

  EXPECT_EQ(SelectVulkanDevice(devices, ""), 1U);
}

TEST(VulkanDeviceSelectionTest, InstanceWithNoDeviceReportsThatNoDeviceExists) {
  EXPECT_EQ(ThrownMessage({}, ""), "Vulkan: the instance has no physical device.");
}

}  // namespace
}  // namespace alcedo::vulkan
