//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/vulkan/vulkan_staging_buffers.hpp"

#include <algorithm>
#include <utility>

#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo {
namespace {

auto AlignUp(VkDeviceSize value, VkDeviceSize alignment) -> VkDeviceSize {
  return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

VulkanStagingBuffers::VulkanStagingBuffers(VmaAllocator allocator, VulkanStagingDirection direction)
    : allocator_(allocator), direction_(direction) {}

VulkanStagingBuffers::~VulkanStagingBuffers() {
  for (auto* list : {&recording_, &submitted_, &free_}) {
    for (auto& chunk : *list) {
      DestroyChunk(chunk);
    }
  }
}

auto VulkanStagingBuffers::Allocate(VkDeviceSize bytes) -> VulkanStagingRange {
  if (!recording_.empty()) {
    auto&      chunk = recording_.back();
    const auto start = AlignUp(chunk.offset, kRangeAlignment);
    if (start + bytes <= chunk.size) {
      chunk.offset = start + bytes;
      return {chunk.buffer, start, bytes, chunk.mapped + start, chunk.allocation};
    }
  }
  // The smallest free chunk that holds the range.
  auto best = free_.end();
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->size >= bytes && (best == free_.end() || it->size < best->size)) {
      best = it;
    }
  }
  Chunk chunk;
  if (best != free_.end()) {
    chunk = *best;
    free_.erase(best);
  } else {
    chunk = CreateChunk(std::max(kMinChunkBytes, AlignUp(bytes, VkDeviceSize{1} << 20)));
  }
  chunk.offset = bytes;
  recording_.push_back(chunk);
  return {chunk.buffer, 0, bytes, chunk.mapped, chunk.allocation};
}

void VulkanStagingBuffers::CloseRecording(std::uint64_t batch_serial) {
  for (auto& chunk : recording_) {
    chunk.batch_serial = batch_serial;
    submitted_.push_back(chunk);
  }
  recording_.clear();
}

void VulkanStagingBuffers::DiscardRecording() noexcept {
  for (auto& chunk : recording_) {
    ReturnToFreeList(chunk);
  }
  recording_.clear();
}

void VulkanStagingBuffers::Recycle(std::uint64_t completed_serial) noexcept {
  auto keep = submitted_.begin();
  for (auto& chunk : submitted_) {
    if (chunk.batch_serial <= completed_serial) {
      ReturnToFreeList(chunk);
    } else {
      *keep++ = chunk;
    }
  }
  submitted_.erase(keep, submitted_.end());
}

auto VulkanStagingBuffers::CreateChunk(VkDeviceSize bytes) -> Chunk {
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size  = bytes;
  buffer_info.usage = direction_ == VulkanStagingDirection::Upload
                          ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                          : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          (direction_ == VulkanStagingDirection::Upload
                               ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                               : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
  Chunk             chunk;
  VmaAllocationInfo result{};
  vulkan::ThrowIfVulkanError(vmaCreateBuffer(allocator_, &buffer_info, &allocation_info,
                                             &chunk.buffer, &chunk.allocation, &result),
                             "vmaCreateBuffer (staging)");
  chunk.mapped = static_cast<std::byte*>(result.pMappedData);
  chunk.size   = bytes;
  ++created_count_;
  vulkan::NoteVulkanBufferCreation();
  return chunk;
}

void VulkanStagingBuffers::DestroyChunk(Chunk& chunk) noexcept {
  if (chunk.buffer != VK_NULL_HANDLE) {
    vmaDestroyBuffer(allocator_, chunk.buffer, chunk.allocation);
    vulkan::NoteVulkanResourceDestruction();
  }
  chunk = {};
}

void VulkanStagingBuffers::ReturnToFreeList(Chunk chunk) noexcept {
  if (chunk.size > kKeptChunkBytes) {
    DestroyChunk(chunk);
    return;
  }
  chunk.offset       = 0;
  chunk.batch_serial = 0;
  try {
    free_.push_back(chunk);
  } catch (...) {
    // No memory for the list entry: the chunk is destroyed instead of kept for reuse.
    DestroyChunk(chunk);
  }
}

}  // namespace alcedo
