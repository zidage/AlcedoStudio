//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// VulkanContext on the GPUs of the local computer. Each test creates the context with the
// validation layer (when it is installed) and checks that the layer reports no error.

#include "vulkan/vulkan_context.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vulkan/vulkan_requirements.hpp"

namespace alcedo::vulkan {
namespace {

// One physical device as a separate instance of the test sees it.
struct LocalDevice {
  std::string             name;
  VkPhysicalDeviceType    type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  /// The first family with graphics, compute, and timestamps, and its queue count.
  std::optional<uint32_t> pipeline_family;
  uint32_t                pipeline_family_queue_count = 0;
};

// Lists the physical devices through an instance of the test. The test has its own volk copy:
// the context can be a DLL that does not export its volk function pointers.
auto ListLocalDevices() -> std::vector<LocalDevice> {
  if (volkInitialize() != VK_SUCCESS) {
    return {};
  }
  VkApplicationInfo application{};
  application.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo create_info{};
  create_info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;
  VkInstance instance          = VK_NULL_HANDLE;
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
    volkFinalize();
    return {};
  }
  volkLoadInstanceOnly(instance);

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> physical_devices(count);
  vkEnumeratePhysicalDevices(instance, &count, physical_devices.data());

  std::vector<LocalDevice> devices;
  for (const VkPhysicalDevice physical_device : physical_devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    LocalDevice device;
    device.name           = properties.deviceName;
    device.type           = properties.deviceType;

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    constexpr VkQueueFlags kRequired = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t index = 0; index < family_count; ++index) {
      if ((families[index].queueFlags & kRequired) == kRequired &&
          families[index].timestampValidBits > 0) {
        device.pipeline_family             = index;
        device.pipeline_family_queue_count = families[index].queueCount;
        break;
      }
    }
    devices.push_back(device);
  }
  vkDestroyInstance(instance, nullptr);
  volkFinalize();
  return devices;
}

auto ValidationLayerExpected() -> bool {
  // The Vulkan SDK installs VK_LAYER_KHRONOS_validation and sets VULKAN_SDK.
#if defined(_WIN32)
  char*       value  = nullptr;
  std::size_t length = 0;
  const bool  found  = _dupenv_s(&value, &length, "VULKAN_SDK") == 0 && value != nullptr;
  std::free(value);
  return found;
#else
  return std::getenv("VULKAN_SDK") != nullptr;
#endif
}

auto OptionsWithValidation(std::string device_name_filter) -> VulkanInitializationOptions {
  VulkanInitializationOptions options;
  options.device_name_filter      = std::move(device_name_filter);
  options.enable_validation_layer = true;
  return options;
}

// The device of this run: ALCEDO_VULKAN_DEVICE selects it (for example "Iris" for the Iris Xe
// run), or the default rule without the variable.
auto OptionsOfThisRun() -> VulkanInitializationOptions {
  return OptionsWithValidation(VulkanInitializationOptions::FromEnvironment().device_name_filter);
}

class VulkanContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(VulkanContext::IsCreated());
    validation_errors_before_ = VulkanContext::ValidationErrorCount();
  }

  void TearDown() override {
    VulkanContext::Destroy();
    EXPECT_FALSE(VulkanContext::IsCreated());
    // Covers the creation and the destruction of the instance and the device.
    EXPECT_EQ(VulkanContext::ValidationErrorCount(),
              validation_errors_before_ + expected_validation_errors_)
        << "The validation layer reported errors; the alcedo.gpu log has the messages.";
  }

  /// The validation errors that a test makes on purpose.
  uint64_t expected_validation_errors_ = 0;

 private:
  uint64_t validation_errors_before_ = 0;
};

TEST_F(VulkanContextTest, CreateOnLocalGpuReportsVulkan13AndAComputeQueue) {
  VulkanContext& context = VulkanContext::Create(OptionsOfThisRun());

  EXPECT_EQ(&VulkanContext::Instance(), &context);
  EXPECT_EQ(context.ValidationLayerEnabled(), ValidationLayerExpected());

  const auto& capabilities = context.Capabilities();
  EXPECT_FALSE(capabilities.name.empty());
  EXPECT_GE(capabilities.api_version, VK_API_VERSION_1_3);
  EXPECT_TRUE(CheckVulkanDeviceRequirements(capabilities).Passed());
  EXPECT_GT(capabilities.device_local_heap_bytes, 0U);

  const auto& queue = context.PipelineQueue();
  EXPECT_NE(queue.device, VK_NULL_HANDLE);
  EXPECT_NE(queue.queue, VK_NULL_HANDLE);
  EXPECT_EQ(queue.queue_index, 0U);
  ASSERT_LT(queue.family_index, capabilities.queue_families.size());
  const auto& family = capabilities.queue_families[queue.family_index];
  EXPECT_NE(family.flags & VK_QUEUE_COMPUTE_BIT, 0U);
  EXPECT_NE(family.flags & VK_QUEUE_GRAPHICS_BIT, 0U);
  EXPECT_GT(family.timestamp_valid_bits, 0U);

  const auto& extensions = context.PipelineDeviceExtensions();
  EXPECT_TRUE(std::is_sorted(extensions.begin(), extensions.end()));
  for (const char* required : kRequiredDeviceExtensions) {
    EXPECT_NE(std::find(extensions.begin(), extensions.end(), required), extensions.end())
        << required;
  }

  // The allocator reads the budget of VK_EXT_memory_budget for the device-local heap.
  ASSERT_NE(context.PipelineAllocator(), VK_NULL_HANDLE);
  const VkPhysicalDeviceMemoryProperties* memory = nullptr;
  vmaGetMemoryProperties(context.PipelineAllocator(), &memory);
  ASSERT_NE(memory, nullptr);
  std::vector<VmaBudget> budgets(memory->memoryHeapCount);
  vmaGetHeapBudgets(context.PipelineAllocator(), budgets.data());
  uint64_t device_local_budget = 0;
  for (uint32_t heap = 0; heap < memory->memoryHeapCount; ++heap) {
    if ((memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
      device_local_budget = std::max<uint64_t>(device_local_budget, budgets[heap].budget);
    }
  }
  EXPECT_GT(device_local_budget, 0U);

  // The device-level functions come from the device table of the context.
  ASSERT_NE(context.PipelineDeviceFunctions().vkQueueWaitIdle, nullptr);
  EXPECT_EQ(context.PipelineDeviceFunctions().vkQueueWaitIdle(queue.queue), VK_SUCCESS);
}

TEST_F(VulkanContextTest, DeviceNameFilterSelectsTheMatchingDevice) {
  const auto devices = ListLocalDevices();
  ASSERT_FALSE(devices.empty());

  const std::string default_name = VulkanContext::Create(OptionsOfThisRun()).Capabilities().name;
  VulkanContext::Destroy();

  // Select a device other than the default one when the computer has one with a pipeline queue.
  std::string filter = default_name;
  for (const auto& device : devices) {
    if (device.name != default_name && device.pipeline_family.has_value()) {
      filter = device.name;
      break;
    }
  }

  const VulkanContext& context = VulkanContext::Create(OptionsWithValidation(filter));

  EXPECT_EQ(context.Capabilities().name, filter);
  RecordProperty("default_device", default_name);
  RecordProperty("filtered_device", filter);
}

TEST_F(VulkanContextTest, SingleQueueDeviceGivesThePipelineQueueZeroOfItsGraphicsFamily) {
  const auto devices = ListLocalDevices();
  const auto single_queue_device =
      std::find_if(devices.begin(), devices.end(), [](const LocalDevice& device) {
        return device.pipeline_family.has_value() && device.pipeline_family_queue_count == 1;
      });
  if (single_queue_device == devices.end()) {
    GTEST_SKIP() << "No local device has one queue in its graphics family (Iris Xe has one).";
  }

  const VulkanContext& context =
      VulkanContext::Create(OptionsWithValidation(single_queue_device->name));

  // The pipeline device takes the only queue of the family. The display device of phase VK18 is
  // a second VkDevice with its own queue 0 (plan decision B5), so no queue is shared.
  EXPECT_EQ(context.Capabilities().name, single_queue_device->name);
  EXPECT_EQ(context.PipelineQueue().family_index, *single_queue_device->pipeline_family);
  EXPECT_EQ(context.PipelineQueue().queue_index, 0U);
  EXPECT_NE(context.PipelineQueue().queue, VK_NULL_HANDLE);
}

TEST_F(VulkanContextTest, FilterThatMatchesNoDeviceThrowsAndLeavesNoContext) {
  try {
    (void)VulkanContext::Create(OptionsWithValidation("no-such-vulkan-device"));
    FAIL() << "Create did not throw.";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("'no-such-vulkan-device' matches no device"),
              std::string::npos)
        << error.what();
  }
  EXPECT_FALSE(VulkanContext::IsCreated());
  EXPECT_THROW((void)VulkanContext::Instance(), std::logic_error);

  // A failed Create leaves nothing behind: a later Create runs again.
  const VulkanContext& context = VulkanContext::Create(OptionsOfThisRun());
  EXPECT_NE(context.PipelineQueue().queue, VK_NULL_HANDLE);
}

TEST_F(VulkanContextTest, InvalidApiUseIsCountedAsAValidationError) {
  const VulkanContext& context = VulkanContext::Create(OptionsOfThisRun());
  if (!context.ValidationLayerEnabled()) {
    GTEST_SKIP() << "VK_LAYER_KHRONOS_validation is not installed.";
  }
  const uint64_t    before = VulkanContext::ValidationErrorCount();

  // A wrong sType is a validation error (VUID-VkFenceCreateInfo-sType-sType). The two structures
  // have the same members (sType, pNext, flags), so the driver creates a normal fence.
  VkFenceCreateInfo info{};
  info.sType               = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFence        fence     = VK_NULL_HANDLE;
  const auto&    functions = context.PipelineDeviceFunctions();
  const VkDevice device    = context.PipelineQueue().device;
  ASSERT_EQ(functions.vkCreateFence(device, &info, nullptr, &fence), VK_SUCCESS);
  functions.vkDestroyFence(device, fence, nullptr);

  EXPECT_GE(VulkanContext::ValidationErrorCount(), before + 1);
  expected_validation_errors_ = VulkanContext::ValidationErrorCount() - before;
}

TEST_F(VulkanContextTest, SecondCreateThrowsAndKeepsTheFirstContext) {
  VulkanContext& first  = VulkanContext::Create(OptionsOfThisRun());
  const VkDevice device = first.PipelineQueue().device;

  EXPECT_THROW((void)VulkanContext::Create(OptionsOfThisRun()), std::logic_error);
  EXPECT_EQ(&VulkanContext::InstanceOrCreate(OptionsWithValidation("no-such-vulkan-device")),
            &first);
  EXPECT_EQ(VulkanContext::Instance().PipelineQueue().device, device);
}

}  // namespace
}  // namespace alcedo::vulkan
