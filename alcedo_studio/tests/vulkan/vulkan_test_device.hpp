//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Support of the Vulkan tests that run kernels on a local GPU: the context of the test run, a
// check that the validation layer reports no error, host-visible buffers, storage images, and one
// submission of recorded commands. The render backend of VK2 replaces these helpers in the
// product; they exist so that VK1 can prove that a pipeline from the cache runs.

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "vulkan/vulkan_context.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo::vulkan::test_support {

/// The options of the run: validation on, and the device of `ALCEDO_VULKAN_DEVICE` (for example
/// "Iris" for the Iris Xe run) or of the default rule.
inline auto OptionsOfThisRun() -> VulkanInitializationOptions {
  VulkanInitializationOptions options;
  options.device_name_filter = VulkanInitializationOptions::FromEnvironment().device_name_filter;
  options.enable_validation_layer = true;
  return options;
}

/// A new empty folder under the temporary folder of the system, removed by the destructor.
class TemporaryFolder {
 public:
  explicit TemporaryFolder(const std::string& name) {
    path_ = std::filesystem::temp_directory_path() / ("alcedo_vulkan_test_" + name);
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TemporaryFolder() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TemporaryFolder(const TemporaryFolder&)                                  = delete;
  auto               operator=(const TemporaryFolder&) -> TemporaryFolder& = delete;

  [[nodiscard]] auto Path() const -> const std::filesystem::path& { return path_; }

 private:
  std::filesystem::path path_;
};

/**
 * @brief Base of a test that uses `VulkanContext`. Each test creates the context; `TearDown`
 * destroys it and checks that the validation layer reported no error from creation to
 * destruction.
 */
class VulkanContextTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(VulkanContext::IsCreated());
    validation_errors_before_ = VulkanContext::ValidationErrorCount();
  }

  void TearDown() override {
    VulkanContext::Destroy();
    EXPECT_EQ(VulkanContext::ValidationErrorCount(), validation_errors_before_)
        << "The validation layer reported errors; the alcedo.gpu log has the messages.";
  }

 private:
  uint64_t validation_errors_before_ = 0;
};

/// A buffer with a device address in host-visible memory of the pipeline device.
class HostVisibleBuffer {
 public:
  HostVisibleBuffer(VulkanContext& context, VkDeviceSize size) : context_(&context) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size  = size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo result{};
    ThrowIfVulkanError(vmaCreateBuffer(context.PipelineAllocator(), &buffer_info, &allocation_info,
                                       &buffer_, &allocation_, &result),
                       "vmaCreateBuffer");
    mapped_ = result.pMappedData;

    VkBufferDeviceAddressInfo address_info{};
    address_info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address_info.buffer = buffer_;
    address_            = context.PipelineDeviceFunctions().vkGetBufferDeviceAddress(
        context.PipelineQueue().device, &address_info);
  }
  ~HostVisibleBuffer() { vmaDestroyBuffer(context_->PipelineAllocator(), buffer_, allocation_); }
  HostVisibleBuffer(const HostVisibleBuffer&)                                  = delete;
  auto               operator=(const HostVisibleBuffer&) -> HostVisibleBuffer& = delete;

  [[nodiscard]] auto Buffer() const -> VkBuffer { return buffer_; }
  [[nodiscard]] auto Address() const -> VkDeviceAddress { return address_; }
  /// The mapped memory. Call `InvalidateForHostRead` after the GPU wrote it.
  [[nodiscard]] auto Mapped() const -> void* { return mapped_; }
  void               InvalidateForHostRead() const {
    ThrowIfVulkanError(
        vmaInvalidateAllocation(context_->PipelineAllocator(), allocation_, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  }

 private:
  VulkanContext*  context_;
  VkBuffer        buffer_     = VK_NULL_HANDLE;
  VmaAllocation   allocation_ = VK_NULL_HANDLE;
  void*           mapped_     = nullptr;
  VkDeviceAddress address_    = 0;
};

/// An RGBA32F storage image of the pipeline device with its view.
class StorageImage {
 public:
  StorageImage(VulkanContext& context, uint32_t width, uint32_t height) : context_(&context) {
    VkImageCreateInfo image_info{};
    image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType     = VK_IMAGE_TYPE_2D;
    image_info.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
    image_info.extent        = {width, height, 1};
    image_info.mipLevels     = 1;
    image_info.arrayLayers   = 1;
    image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ThrowIfVulkanError(vmaCreateImage(context.PipelineAllocator(), &image_info, &allocation_info,
                                      &image_, &allocation_, nullptr),
                       "vmaCreateImage");

    VkImageViewCreateInfo view_info{};
    view_info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image            = image_;
    view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format           = VK_FORMAT_R32G32B32A32_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ThrowIfVulkanError(context.PipelineDeviceFunctions().vkCreateImageView(
                           context.PipelineQueue().device, &view_info, nullptr, &view_),
                       "vkCreateImageView");
  }
  ~StorageImage() {
    context_->PipelineDeviceFunctions().vkDestroyImageView(context_->PipelineQueue().device, view_,
                                                           nullptr);
    vmaDestroyImage(context_->PipelineAllocator(), image_, allocation_);
  }
  StorageImage(const StorageImage&)                                  = delete;
  auto               operator=(const StorageImage&) -> StorageImage& = delete;

  [[nodiscard]] auto Image() const -> VkImage { return image_; }
  [[nodiscard]] auto View() const -> VkImageView { return view_; }

 private:
  VulkanContext* context_;
  VkImage        image_      = VK_NULL_HANDLE;
  VmaAllocation  allocation_ = VK_NULL_HANDLE;
  VkImageView    view_       = VK_NULL_HANDLE;
};

/// Records commands with @p record into one command buffer, submits it to the pipeline queue, and
/// waits for its fence.
inline void SubmitAndWait(VulkanContext&                              context,
                          const std::function<void(VkCommandBuffer)>& record) {
  const auto&             functions = context.PipelineDeviceFunctions();
  const auto&             queue     = context.PipelineQueue();

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = queue.family_index;
  VkCommandPool pool         = VK_NULL_HANDLE;
  ThrowIfVulkanError(functions.vkCreateCommandPool(queue.device, &pool_info, nullptr, &pool),
                     "vkCreateCommandPool");
  VkFence fence = VK_NULL_HANDLE;
  try {
    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool        = pool;
    allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer   = VK_NULL_HANDLE;
    ThrowIfVulkanError(
        functions.vkAllocateCommandBuffers(queue.device, &allocate_info, &command_buffer),
        "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ThrowIfVulkanError(functions.vkBeginCommandBuffer(command_buffer, &begin_info),
                       "vkBeginCommandBuffer");
    record(command_buffer);
    ThrowIfVulkanError(functions.vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ThrowIfVulkanError(functions.vkCreateFence(queue.device, &fence_info, nullptr, &fence),
                       "vkCreateFence");
    VkCommandBufferSubmitInfo command_info{};
    command_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = command_buffer;
    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &command_info;
    ThrowIfVulkanError(functions.vkQueueSubmit2(queue.queue, 1, &submit, fence), "vkQueueSubmit2");
    ThrowIfVulkanError(functions.vkWaitForFences(queue.device, 1, &fence, VK_TRUE, UINT64_MAX),
                       "vkWaitForFences");
  } catch (...) {
    functions.vkDestroyFence(queue.device, fence, nullptr);
    functions.vkDestroyCommandPool(queue.device, pool, nullptr);
    throw;
  }
  functions.vkDestroyFence(queue.device, fence, nullptr);
  functions.vkDestroyCommandPool(queue.device, pool, nullptr);
}

/// A memory barrier from compute shader writes to @p dst_stage / @p dst_access.
inline void ComputeWriteBarrier(const VolkDeviceTable& functions, VkCommandBuffer command_buffer,
                                VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  barrier.dstStageMask  = dst_stage;
  barrier.dstAccessMask = dst_access;
  VkDependencyInfo dependency{};
  dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers    = &barrier;
  functions.vkCmdPipelineBarrier2(command_buffer, &dependency);
}

/// An image layout transition of the whole color image.
inline void TransitionImage(const VolkDeviceTable& functions, VkCommandBuffer command_buffer,
                            VkImage image, VkImageLayout from, VkImageLayout to,
                            VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask        = src_stage;
  barrier.srcAccessMask       = src_access;
  barrier.dstStageMask        = dst_stage;
  barrier.dstAccessMask       = dst_access;
  barrier.oldLayout           = from;
  barrier.newLayout           = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image               = image;
  barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{};
  dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers    = &barrier;
  functions.vkCmdPipelineBarrier2(command_buffer, &dependency);
}

}  // namespace alcedo::vulkan::test_support
