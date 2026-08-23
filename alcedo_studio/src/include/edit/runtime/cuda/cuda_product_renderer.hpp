//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/static_execution_plan_cache.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class CudaRenderDevice;
class ImageBuffer;

/** @brief Select whether a render may read or publish the editor session caches. */
enum class CudaProductCachePolicy {
  UseSessionCache,
  BypassSessionCache,
};

/// CUDA DAG backend capability mixed into the static plan key. Bump when pass
/// lists or backend traits that affect compilation change.
inline constexpr std::uint32_t kCudaDagBackendCapabilityVersion = 1;

/**
 * @brief Queryable prepare/compile and result-cache counters for one product session.
 */
struct CudaProductSessionStats {
  std::uint64_t    prepared_source_hits     = 0;
  std::uint64_t    prepared_source_misses   = 0;
  std::uint64_t    libraw_open_unpack_count = 0;
  std::uint64_t    plan_cache_hits          = 0;
  std::uint64_t    plan_cache_misses        = 0;
  std::uint64_t    plan_compile_count       = 0;
  GpuNodePassStats pass{};
};

/**
 * @brief Live GPU/host allocations owned by one product session.
 */
struct CudaProductSessionResources {
  std::size_t               published_result_count      = 0;
  std::size_t               texture_pool_used_bytes     = 0;
  std::size_t               texture_pool_entry_count    = 0;
  std::size_t               prepared_source_host_bytes  = 0;
  std::size_t               prepared_source_entry_count = 0;
  std::vector<GraphValueId> session_value_ids;
};

/**
 * @brief Reusable CUDA product session for one opened PipelineDocument.
 *
 * Owns the editor session caches/device plus a lazily created one-shot device for
 * thumbnail and export work. Not created per Apply. Only session renders reuse
 * prepared sources, static plans, and published GPU results.
 */
class CudaProductRenderer {
 public:
  explicit CudaProductRenderer(std::shared_ptr<PipelineDocument> document);
  CudaProductRenderer(std::shared_ptr<PipelineDocument> document,
                      PreparedSourceCache::UnpackFn     unpack);
  ~CudaProductRenderer();

  CudaProductRenderer(const CudaProductRenderer&)                                  = delete;
  auto               operator=(const CudaProductRenderer&) -> CudaProductRenderer& = delete;

  void               SetDocument(std::shared_ptr<PipelineDocument> document);

  /**
   * @brief Render one frame on the owning thread.
   *
   * @param cache_policy Session mode reads and publishes reusable editor results. Bypass mode
   *        uses an isolated one-shot workspace and releases its result resources after delivery.
   * @throws std::runtime_error for invalid input, CUDA execution, or presentation failure.
   */
  [[nodiscard]] auto Render(
      const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res, const RenderRequest& request,
      IFrameSink* sink, const FrameCompletionSubmission& submission, bool require_host_output,
      CudaProductCachePolicy cache_policy = CudaProductCachePolicy::UseSessionCache)
      -> std::shared_ptr<ImageBuffer>;

  /**
   * @brief Snapshot of source and static-plan cache counters since construction or ResetStats.
   */
  [[nodiscard]] auto Stats() const -> CudaProductSessionStats;
  void               ResetStats();

  /**
   * @brief Drop GPU result textures, host prepared sources, the plan cache, and
   *        the session Neural workspace. Keeps the CudaRenderDevice.
   *
   * Call when the pipeline is returned to PipelineMgmtService. The next Render
   * rebuilds caches from the still-owned PipelineDocument.
   */
  void               ReleaseSessionCaches();

  [[nodiscard]] auto SessionResources() const -> CudaProductSessionResources;

  [[nodiscard]] auto Device() -> CudaRenderDevice& { return *device_; }
  [[nodiscard]] auto Device() const -> const CudaRenderDevice& { return *device_; }

  [[nodiscard]] auto SourceCache() -> PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto SourceCache() const -> const PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto PlanCache() -> StaticExecutionPlanCache& { return plan_cache_; }
  [[nodiscard]] auto PlanCache() const -> const StaticExecutionPlanCache& { return plan_cache_; }

 private:
  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<CudaRenderDevice> device_;
  std::unique_ptr<CudaRenderDevice> one_shot_device_;
  PreparedSourceCache::UnpackFn     unpack_;
  PreparedSourceCache               source_cache_;
  StaticExecutionPlanCache          plan_cache_{kCudaDagBackendCapabilityVersion};
};

}  // namespace alcedo
