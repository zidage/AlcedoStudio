//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

// clang-format off
#include <volk.h>
#include <vma/vk_mem_alloc.h>
// clang-format on

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "edit/runtime/vulkan/vulkan_staging_buffers.hpp"
#include "vulkan/vulkan_context.hpp"

namespace alcedo {

// vulkan_backend.hpp includes this header (VulkanBackend::CommandContext); the context keeps a
// pointer to the backend of its open recording only. vulkan_command_context.cpp includes the
// defining header.
class VulkanBackend;

/**
 * @brief The recording and submission state of the renders of one render device (plan section
 * 4.1).
 *
 * A render records its commands into one command buffer. `VulkanBackend::Submit` ends it and
 * submits it as one batch with a fence. A host download submits the recorded commands as a batch
 * of their own and waits for it, because the host needs the bytes at once (the same as the
 * blocking read of the OpenCL backend).
 *
 * The context owns:
 * - the command pool, the command buffers, and the fences; a completed batch returns its command
 *   buffer and its fence for reuse, so a stable render creates neither;
 * - the image layout record of the open recording: the images whose first use the recording
 *   moves from `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL`. `VulkanBackend` adds
 *   them to its record of initialized images only when the batch is submitted;
 * - the staging memory of the uploads and the downloads (`VulkanStagingBuffers`);
 * - the barrier state: a new batch starts with a memory barrier that covers the earlier
 *   submissions of the queue, and a command after a write starts with a memory barrier.
 *
 * Every image of the pipeline is in `VK_IMAGE_LAYOUT_GENERAL` after its first use, so a barrier
 * of the context is a global memory barrier and no layout changes between commands.
 *
 * The context uses `VulkanContext::Instance()` from its construction to its destruction. The
 * destructor waits for the submitted batches and drops a recording that was not submitted.
 * Not thread-safe; one render thread uses it.
 */
class VulkanCommandContext {
 public:
  /// @throws std::logic_error Without a `VulkanContext`. @throws VulkanError From the pool.
  VulkanCommandContext();
  ~VulkanCommandContext();
  VulkanCommandContext(const VulkanCommandContext&)                             = delete;
  auto operator=(const VulkanCommandContext&) -> VulkanCommandContext&          = delete;
  VulkanCommandContext(VulkanCommandContext&&)                                  = delete;
  auto               operator=(VulkanCommandContext&&) -> VulkanCommandContext& = delete;

  [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
  void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }

  /// True while a command buffer records commands that are not submitted.
  [[nodiscard]] auto IsRecording() const -> bool { return recording_ != VK_NULL_HANDLE; }
  /// Batches that are submitted and that no wait has completed yet.
  [[nodiscard]] auto SubmittedBatchCount() const -> std::size_t { return batches_.size(); }

  /**
   * @brief Marks that the last recorded command wrote memory. The next command of the recording
   * starts with a memory barrier.
   */
  void               NoteWrite() { unsynchronized_write_ = true; }

  [[nodiscard]] auto CommandBufferCreateCount() const -> std::uint64_t {
    return command_buffer_create_count_;
  }
  [[nodiscard]] auto FenceCreateCount() const -> std::uint64_t { return fence_create_count_; }
  /// Staging buffers of both directions that this context created.
  [[nodiscard]] auto StagingBufferCreateCount() const -> std::uint64_t {
    return upload_staging_.CreatedCount() + download_staging_.CreatedCount();
  }
  void ResetCounters();

 private:
  friend class VulkanBackend;

  struct Batch {
    std::uint64_t   serial         = 0;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence         fence          = VK_NULL_HANDLE;
  };

  /**
   * @brief The command buffer of the open recording; begins one when none is open, with a memory
   * barrier first. Records a memory barrier before the command when an earlier command of the
   * recording wrote memory.
   */
  [[nodiscard]] auto BeginCommand(VulkanBackend& backend) -> VkCommandBuffer;
  /// Records the move of @p image from `UNDEFINED` to `GENERAL` once in the recording.
  void               InitializeImageLayout(VkImage image, std::uint64_t resource_id);
  /// True when the open recording moves the image @p resource_id to `GENERAL`.
  [[nodiscard]] auto RecordsLayoutOf(std::uint64_t resource_id) const -> bool {
    return recorded_layouts_.contains(resource_id);
  }
  /// Records a barrier from the copy writes to host reads before a download.
  void                   RecordHostReadBarrier();

  /**
   * @brief Ends the recording and submits it with a fence as batch @p serial.
   * @return The resource ids whose layout the batch moved to `GENERAL`.
   * @throws VulkanError When `vkEndCommandBuffer` or the submission fails. The recording is then
   *         dropped; no batch exists for it.
   */
  auto                   SubmitRecording(std::uint64_t serial) -> std::vector<std::uint64_t>;
  /// Drops the open recording without a submission. Its staging ranges return to the free list.
  void                   DropRecording() noexcept;
  /**
   * @brief Waits for the fence of each submitted batch and takes back their command buffers and
   * fences. The staging memory of the batches stays until @ref RecycleStaging, so a download can
   * copy its bytes first.
   * @return The serial of the last batch, or 0 when no batch was submitted.
   * @throws VulkanError When a wait fails (for example `VK_ERROR_DEVICE_LOST`). The batches stay.
   */
  auto                   WaitSubmittedBatches() -> std::uint64_t;
  /// The staging memory of the batches up to @p completed_serial returns for reuse.
  void                   RecycleStaging(std::uint64_t completed_serial) noexcept;

  [[nodiscard]] auto     AcquireCommandBuffer() -> VkCommandBuffer;
  [[nodiscard]] auto     AcquireFence() -> VkFence;
  void                   RecordMemoryBarrier(VkCommandBuffer command_buffer);

  vulkan::VulkanContext* context_;
  const VolkDeviceTable* functions_;
  VkDevice               device_;
  VkCommandPool          pool_                 = VK_NULL_HANDLE;
  VkCommandBuffer        recording_            = VK_NULL_HANDLE;
  /// The backend whose resources the open recording uses.
  VulkanBackend*         recording_backend_    = nullptr;
  bool                   unsynchronized_write_ = false;
  std::unordered_set<std::uint64_t> recorded_layouts_;
  std::vector<Batch>                batches_;
  std::vector<VkCommandBuffer>      free_command_buffers_;
  std::vector<VkFence>              free_fences_;
  VulkanStagingBuffers              upload_staging_;
  VulkanStagingBuffers              download_staging_;
  std::uint64_t                     submission_id_               = 0;
  std::uint64_t                     command_buffer_create_count_ = 0;
  std::uint64_t                     fence_create_count_          = 0;
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
