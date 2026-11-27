//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/vulkan/vulkan_command_context.hpp"

#include <cstdint>
#include <limits>
#include <utility>

#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "utils/logging/log.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo {

using vulkan::ThrowIfVulkanError;

VulkanCommandContext::VulkanCommandContext()
    : context_(&vulkan::VulkanContext::Instance()),
      functions_(&context_->PipelineDeviceFunctions()),
      device_(context_->PipelineQueue().device),
      upload_staging_(context_->PipelineAllocator(), VulkanStagingDirection::Upload),
      download_staging_(context_->PipelineAllocator(), VulkanStagingDirection::Download) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = context_->PipelineQueue().family_index;
  ThrowIfVulkanError(functions_->vkCreateCommandPool(device_, &pool_info, nullptr, &pool_),
                     "vkCreateCommandPool");
}

VulkanCommandContext::~VulkanCommandContext() {
  try {
    (void)WaitSubmittedBatches();
  } catch (const std::exception& error) {
    // A lost device: the driver completes nothing more, and the objects are destroyed anyway.
    ALCEDO_LOG_ERROR(Gpu) << "VulkanCommandContext: the wait at destruction failed: "
                          << error.what();
  }
  DropRecording();
  for (const auto& batch : batches_) {
    free_fences_.push_back(batch.fence);
  }
  batches_.clear();
  for (const VkFence fence : free_fences_) {
    functions_->vkDestroyFence(device_, fence, nullptr);
  }
  // Destroying the pool frees its command buffers.
  functions_->vkDestroyCommandPool(device_, pool_, nullptr);
}

void VulkanCommandContext::ResetCounters() {
  command_buffer_create_count_ = 0;
  fence_create_count_          = 0;
  upload_staging_.ResetCreatedCount();
  download_staging_.ResetCreatedCount();
}

auto VulkanCommandContext::BeginCommand(VulkanBackend& backend) -> VkCommandBuffer {
  if (recording_ != VK_NULL_HANDLE) {
    if (recording_backend_ != &backend) {
      throw std::logic_error(
          "VulkanCommandContext: the open recording belongs to another VulkanBackend");
    }
    if (unsynchronized_write_) {
      RecordMemoryBarrier(recording_);
      unsynchronized_write_ = false;
    }
    return recording_;
  }
  // The backend refuses a second recording for itself before any object is taken.
  backend.NoteRecordingOpened(*this);
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  try {
    command_buffer = AcquireCommandBuffer();
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ThrowIfVulkanError(functions_->vkBeginCommandBuffer(command_buffer, &begin_info),
                       "vkBeginCommandBuffer");
  } catch (...) {
    if (command_buffer != VK_NULL_HANDLE) {
      free_command_buffers_.push_back(command_buffer);
    }
    backend.NoteRecordingClosed(*this);
    throw;
  }
  // The first command of a batch waits for the writes of the earlier submissions of the queue.
  RecordMemoryBarrier(command_buffer);
  recording_            = command_buffer;
  recording_backend_    = &backend;
  unsynchronized_write_ = false;
  return recording_;
}

void VulkanCommandContext::InitializeImageLayout(VkImage image, std::uint64_t resource_id) {
  if (recorded_layouts_.contains(resource_id)) {
    return;
  }
  VkImageMemoryBarrier2 barrier{};
  barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
  barrier.srcAccessMask       = VK_ACCESS_2_NONE;
  barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image               = image;
  barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{};
  dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers    = &barrier;
  functions_->vkCmdPipelineBarrier2(recording_, &dependency);
  recorded_layouts_.insert(resource_id);
}

void VulkanCommandContext::RecordHostReadBarrier() {
  VkMemoryBarrier2 barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dependency{};
  dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers    = &barrier;
  functions_->vkCmdPipelineBarrier2(recording_, &dependency);
}

void VulkanCommandContext::RecordMemoryBarrier(VkCommandBuffer command_buffer) {
  VkMemoryBarrier2 barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo dependency{};
  dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers    = &barrier;
  functions_->vkCmdPipelineBarrier2(command_buffer, &dependency);
}

auto VulkanCommandContext::SubmitRecording(std::uint64_t serial) -> std::vector<std::uint64_t> {
  if (recording_ == VK_NULL_HANDLE) {
    return {};
  }
  // Every allocation comes before the submission: after it, nothing may fail.
  std::vector<std::uint64_t> initialized;
  try {
    initialized.assign(recorded_layouts_.begin(), recorded_layouts_.end());
    ThrowIfVulkanError(functions_->vkEndCommandBuffer(recording_), "vkEndCommandBuffer");
    batches_.reserve(batches_.size() + 1);
    const VkFence             fence = AcquireFence();

    VkCommandBufferSubmitInfo command_info{};
    command_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = recording_;
    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &command_info;
    try {
      context_->SubmitToPipelineQueue(submit, fence);
    } catch (...) {
      free_fences_.push_back(fence);
      throw;
    }
    vulkan::NoteVulkanSubmit();
    batches_.push_back(Batch{serial, recording_, fence});
  } catch (...) {
    DropRecording();
    throw;
  }
  upload_staging_.CloseRecording(serial);
  download_staging_.CloseRecording(serial);
  recorded_layouts_.clear();
  recording_backend_->NoteRecordingClosed(*this);
  recording_            = VK_NULL_HANDLE;
  recording_backend_    = nullptr;
  unsynchronized_write_ = false;
  return initialized;
}

void VulkanCommandContext::DropRecording() noexcept {
  if (recording_ == VK_NULL_HANDLE) {
    return;
  }
  (void)functions_->vkResetCommandBuffer(recording_, 0);
  try {
    free_command_buffers_.push_back(recording_);
  } catch (...) {
    functions_->vkFreeCommandBuffers(device_, pool_, 1, &recording_);
  }
  upload_staging_.DiscardRecording();
  download_staging_.DiscardRecording();
  recorded_layouts_.clear();
  if (recording_backend_ != nullptr) {
    recording_backend_->NoteRecordingClosed(*this);
  }
  recording_            = VK_NULL_HANDLE;
  recording_backend_    = nullptr;
  unsynchronized_write_ = false;
}

auto VulkanCommandContext::WaitSubmittedBatches() -> std::uint64_t {
  if (batches_.empty()) {
    return 0;
  }
  // The batches are in submission order: the fence of the last one covers every earlier batch of
  // the queue, but each fence must signal before it is reset for reuse.
  std::vector<VkFence> fences;
  fences.reserve(batches_.size());
  for (const auto& batch : batches_) {
    fences.push_back(batch.fence);
  }
  ThrowIfVulkanError(
      functions_->vkWaitForFences(device_, static_cast<std::uint32_t>(fences.size()), fences.data(),
                                  VK_TRUE, (std::numeric_limits<std::uint64_t>::max)()),
      "vkWaitForFences");
  vulkan::NoteVulkanWait();
  const std::uint64_t last_serial = batches_.back().serial;
  free_command_buffers_.reserve(free_command_buffers_.size() + batches_.size());
  free_fences_.reserve(free_fences_.size() + batches_.size());
  for (const auto& batch : batches_) {
    free_command_buffers_.push_back(batch.command_buffer);
    free_fences_.push_back(batch.fence);
  }
  batches_.clear();
  return last_serial;
}

void VulkanCommandContext::RecycleStaging(std::uint64_t completed_serial) noexcept {
  upload_staging_.Recycle(completed_serial);
  download_staging_.Recycle(completed_serial);
}

auto VulkanCommandContext::AcquireCommandBuffer() -> VkCommandBuffer {
  if (!free_command_buffers_.empty()) {
    const VkCommandBuffer command_buffer = free_command_buffers_.back();
    free_command_buffers_.pop_back();
    return command_buffer;
  }
  VkCommandBufferAllocateInfo allocate_info{};
  allocate_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool        = pool_;
  allocate_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  VkCommandBuffer command_buffer   = VK_NULL_HANDLE;
  ThrowIfVulkanError(functions_->vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer),
                     "vkAllocateCommandBuffers");
  ++command_buffer_create_count_;
  return command_buffer;
}

auto VulkanCommandContext::AcquireFence() -> VkFence {
  if (!free_fences_.empty()) {
    const VkFence fence = free_fences_.back();
    ThrowIfVulkanError(functions_->vkResetFences(device_, 1, &fence), "vkResetFences");
    free_fences_.pop_back();
    return fence;
  }
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence    = VK_NULL_HANDLE;
  ThrowIfVulkanError(functions_->vkCreateFence(device_, &fence_info, nullptr, &fence),
                     "vkCreateFence");
  ++fence_create_count_;
  return fence;
}

}  // namespace alcedo
