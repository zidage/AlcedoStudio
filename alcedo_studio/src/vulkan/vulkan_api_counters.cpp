//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "vulkan/vulkan_api_counters.hpp"

#include <atomic>

namespace alcedo::vulkan {
namespace {

std::atomic<uint64_t> g_module_loads{0};
std::atomic<uint64_t> g_pipeline_creations{0};
std::atomic<uint64_t> g_pipeline_cache_hits{0};
std::atomic<uint64_t> g_dispatches{0};
std::atomic<uint64_t> g_upload_bytes{0};
std::atomic<uint64_t> g_download_bytes{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<uint64_t> g_waits{0};

void                  Add(std::atomic<uint64_t>& counter, uint64_t value) noexcept {
  counter.fetch_add(value, std::memory_order_relaxed);
}

auto Load(const std::atomic<uint64_t>& counter) noexcept -> uint64_t {
  return counter.load(std::memory_order_relaxed);
}

}  // namespace

auto ReadVulkanApiCounters() noexcept -> VulkanApiCounters {
  VulkanApiCounters counters;
  counters.module_loads        = Load(g_module_loads);
  counters.pipeline_creations  = Load(g_pipeline_creations);
  counters.pipeline_cache_hits = Load(g_pipeline_cache_hits);
  counters.dispatches          = Load(g_dispatches);
  counters.upload_bytes        = Load(g_upload_bytes);
  counters.download_bytes      = Load(g_download_bytes);
  counters.submits             = Load(g_submits);
  counters.waits               = Load(g_waits);
  return counters;
}

auto VulkanApiCounterDelta(const VulkanApiCounters& before, const VulkanApiCounters& after) noexcept
    -> VulkanApiCounters {
  VulkanApiCounters delta;
  delta.module_loads        = after.module_loads - before.module_loads;
  delta.pipeline_creations  = after.pipeline_creations - before.pipeline_creations;
  delta.pipeline_cache_hits = after.pipeline_cache_hits - before.pipeline_cache_hits;
  delta.dispatches          = after.dispatches - before.dispatches;
  delta.upload_bytes        = after.upload_bytes - before.upload_bytes;
  delta.download_bytes      = after.download_bytes - before.download_bytes;
  delta.submits             = after.submits - before.submits;
  delta.waits               = after.waits - before.waits;
  return delta;
}

void NoteVulkanModuleLoad() noexcept { Add(g_module_loads, 1); }
void NoteVulkanPipelineCreation() noexcept { Add(g_pipeline_creations, 1); }
void NoteVulkanPipelineCacheHit() noexcept { Add(g_pipeline_cache_hits, 1); }
void NoteVulkanDispatch() noexcept { Add(g_dispatches, 1); }
void NoteVulkanUpload(uint64_t bytes) noexcept { Add(g_upload_bytes, bytes); }
void NoteVulkanDownload(uint64_t bytes) noexcept { Add(g_download_bytes, bytes); }
void NoteVulkanSubmit() noexcept { Add(g_submits, 1); }
void NoteVulkanWait() noexcept { Add(g_waits, 1); }

}  // namespace alcedo::vulkan
