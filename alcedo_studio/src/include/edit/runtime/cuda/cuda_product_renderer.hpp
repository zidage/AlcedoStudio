//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>

#include "edit/geometry/render_request.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/static_execution_plan_cache.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class CudaRenderDevice;
class ImageBuffer;
class PipelineDocument;

/// CUDA DAG backend capability mixed into the static plan key. Bump when pass
/// lists or backend traits that affect compilation change.
inline constexpr std::uint32_t kCudaDagBackendCapabilityVersion = 1;

/**
 * @brief Queryable prepare/compile and result-cache counters for one product session.
 */
struct CudaProductSessionStats {
  std::uint64_t prepared_source_hits       = 0;
  std::uint64_t prepared_source_misses     = 0;
  std::uint64_t libraw_open_unpack_count   = 0;
  std::uint64_t plan_cache_hits            = 0;
  std::uint64_t plan_cache_misses          = 0;
  std::uint64_t plan_compile_count         = 0;
  GpuNodePassStats pass{};
};

/**
 * @brief Reusable CUDA product session for one opened PipelineDocument.
 *
 * Owns PreparedSourceCache, the static ExecutionPlan cache, and one
 * CudaRenderDevice/workspace. Not created per Apply. Preview and export frames
 * that share encoded bytes and DecodeRes reuse the same PreparedRawInput.
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

  [[nodiscard]] auto Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                            const RenderRequest& request, IFrameSink* sink,
                            const FrameCompletionSubmission& submission, bool require_host_output)
      -> std::shared_ptr<ImageBuffer>;

  /**
   * @brief Snapshot of source and static-plan cache counters since construction or ResetStats.
   */
  [[nodiscard]] auto Stats() const -> CudaProductSessionStats;
  void               ResetStats();

  [[nodiscard]] auto SourceCache() -> PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto SourceCache() const -> const PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto PlanCache() -> StaticExecutionPlanCache& { return plan_cache_; }
  [[nodiscard]] auto PlanCache() const -> const StaticExecutionPlanCache& { return plan_cache_; }

 private:
  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<CudaRenderDevice> device_;
  PreparedSourceCache               source_cache_;
  StaticExecutionPlanCache          plan_cache_{kCudaDagBackendCapabilityVersion};
};

}  // namespace alcedo
