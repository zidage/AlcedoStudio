//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

// clang-format off
#include <volk.h>
#include <vma/vk_mem_alloc.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <vector>

namespace alcedo {

/// The direction of the bytes in a staging buffer.
enum class VulkanStagingDirection : std::uint8_t {
  /// Host writes, the GPU reads (uploads). Write-combined host memory.
  Upload,
  /// The GPU writes, the host reads (downloads). Cached host memory.
  Download,
};

/// One range of a host-visible staging buffer.
struct VulkanStagingRange {
  VkBuffer      buffer     = VK_NULL_HANDLE;
  VkDeviceSize  offset     = 0;
  VkDeviceSize  bytes      = 0;
  /// The host address of the first byte of the range.
  std::byte*    mapped     = nullptr;
  VmaAllocation allocation = VK_NULL_HANDLE;
};

/**
 * @brief Host-visible staging memory of one command context, in chunks with a bump offset.
 *
 * A transfer between the host and a device resource goes through a staging range. The chunks
 * that the open recording uses belong to it until its batch is submitted; then they belong to the
 * batch until its fence signals; then they return to the free list and the next recording uses
 * them again. A stable render therefore creates no staging buffer. A chunk larger than
 * @ref kKeptChunkBytes is destroyed when its batch is complete, so a full-size image upload does
 * not keep its staging memory.
 *
 * The bytes of an upload are copied into the chunk when the upload is recorded, so the caller can
 * reuse its storage at once. Staging memory is in system memory
 * (`VMA_MEMORY_USAGE_AUTO_PREFER_HOST`) and is not in the device-local budget of plan section 4.2.
 *
 * Owner: `VulkanCommandContext`. Not thread-safe. The destructor destroys every chunk: the owner
 * waits for its submitted batches first.
 */
class VulkanStagingBuffers {
 public:
  static constexpr VkDeviceSize kMinChunkBytes  = VkDeviceSize{4} << 20;
  static constexpr VkDeviceSize kKeptChunkBytes = VkDeviceSize{64} << 20;
  static constexpr VkDeviceSize kRangeAlignment = 16;

  VulkanStagingBuffers(VmaAllocator allocator, VulkanStagingDirection direction);
  ~VulkanStagingBuffers();
  VulkanStagingBuffers(const VulkanStagingBuffers&)                                  = delete;
  auto               operator=(const VulkanStagingBuffers&) -> VulkanStagingBuffers& = delete;

  /**
   * @brief A range of @p bytes for the open recording, aligned to @ref kRangeAlignment.
   * @throws VulkanError When a new chunk cannot be created.
   */
  [[nodiscard]] auto Allocate(VkDeviceSize bytes) -> VulkanStagingRange;

  /// The chunks of the open recording now belong to the batch @p batch_serial.
  void               CloseRecording(std::uint64_t batch_serial);
  /// The chunks of the open recording return to the free list (its commands were not submitted).
  void               DiscardRecording() noexcept;
  /// The chunks of each batch with a serial up to @p completed_serial return to the free list.
  void               Recycle(std::uint64_t completed_serial) noexcept;

  /// Staging buffers that this object created since its construction or the last reset.
  [[nodiscard]] auto CreatedCount() const -> std::uint64_t { return created_count_; }
  void               ResetCreatedCount() { created_count_ = 0; }

 private:
  struct Chunk {
    VkBuffer      buffer       = VK_NULL_HANDLE;
    VmaAllocation allocation   = VK_NULL_HANDLE;
    std::byte*    mapped       = nullptr;
    VkDeviceSize  size         = 0;
    VkDeviceSize  offset       = 0;
    std::uint64_t batch_serial = 0;
  };

  [[nodiscard]] auto     CreateChunk(VkDeviceSize bytes) -> Chunk;
  void                   DestroyChunk(Chunk& chunk) noexcept;
  void                   ReturnToFreeList(Chunk chunk) noexcept;

  VmaAllocator           allocator_;
  VulkanStagingDirection direction_;
  std::vector<Chunk>     recording_;
  std::vector<Chunk>     submitted_;
  std::vector<Chunk>     free_;
  std::uint64_t          created_count_ = 0;
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
