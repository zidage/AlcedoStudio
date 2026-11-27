//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo::vulkan {

/**
 * @brief Process counters of the Vulkan API operations that a stable render must not repeat.
 *
 * Plan section 4.2: a stable render creates no pipeline, no shader module, no image, and no
 * buffer; tests prove it with these counters, as `OpenClApiCounters` does for OpenCL. The
 * counters are always on: each note is one relaxed atomic increment, and the counted operations
 * cost much more than the increment.
 *
 * Owners: `VulkanShaderModuleRegistry` notes module loads, `VulkanComputePipelineCache` notes
 * pipeline creations and cache hits. The render backend notes dispatches, transfers, submissions,
 * and waits.
 */
struct VulkanApiCounters {
  uint64_t module_loads        = 0;  ///< `vkCreateShaderModule`
  uint64_t pipeline_creations  = 0;  ///< `vkCreateComputePipelines`
  uint64_t pipeline_cache_hits = 0;  ///< pipeline requests that the cache answered
  uint64_t dispatches          = 0;  ///< recorded `vkCmdDispatch` and `vkCmdDispatchBase`
  uint64_t upload_bytes        = 0;  ///< host to device bytes
  uint64_t download_bytes      = 0;  ///< device to host bytes
  uint64_t submits             = 0;  ///< `vkQueueSubmit2`
  uint64_t waits               = 0;  ///< host waits for a fence
};

/// The counters of the process since its start. Thread safe.
[[nodiscard]] auto ReadVulkanApiCounters() noexcept -> VulkanApiCounters;

/// The counts between two reads: each field of @p after minus the field of @p before.
[[nodiscard]] auto VulkanApiCounterDelta(const VulkanApiCounters& before,
                                         const VulkanApiCounters& after) noexcept
    -> VulkanApiCounters;

void NoteVulkanModuleLoad() noexcept;
void NoteVulkanPipelineCreation() noexcept;
void NoteVulkanPipelineCacheHit() noexcept;
void NoteVulkanDispatch() noexcept;
void NoteVulkanUpload(uint64_t bytes) noexcept;
void NoteVulkanDownload(uint64_t bytes) noexcept;
void NoteVulkanSubmit() noexcept;
void NoteVulkanWait() noexcept;

}  // namespace alcedo::vulkan
