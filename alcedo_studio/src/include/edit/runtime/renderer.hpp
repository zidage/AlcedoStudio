//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/render_device_type.hpp"
#include "edit/runtime/static_execution_plan_cache.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class ImageBuffer;

/** @brief Select whether a render may read or publish the editor session caches. */
enum class RenderCachePolicy {
  UseSessionCache,
  BypassSessionCache,
};

using CudaProductCachePolicy = RenderCachePolicy;

/**
 * @brief Queryable prepare/compile and result-cache counters for one product session.
 */
struct RenderSessionStats {
  std::uint64_t    prepared_source_hits     = 0;
  std::uint64_t    prepared_source_misses   = 0;
  std::uint64_t    libraw_open_unpack_count = 0;
  std::uint64_t    plan_cache_hits          = 0;
  std::uint64_t    plan_cache_misses        = 0;
  std::uint64_t    plan_compile_count       = 0;
  GpuNodePassStats pass{};
};

using CudaProductSessionStats = RenderSessionStats;

/**
 * @brief Live GPU/host allocations owned by one product session.
 */
struct RenderSessionResources {
  std::size_t               published_result_count      = 0;
  std::size_t               texture_pool_used_bytes     = 0;
  std::size_t               texture_pool_entry_count    = 0;
  std::size_t               prepared_source_host_bytes  = 0;
  std::size_t               prepared_source_entry_count = 0;
  std::vector<GraphValueId> session_value_ids;
};

using CudaProductSessionResources = RenderSessionResources;

/**
 * @brief Reusable product session for one opened PipelineDocument.
 *
 * Owns editor session caches/device plus a lazily created one-shot device for
 * thumbnail and export work. Not created per Apply. Only session renders reuse
 * prepared sources, static plans, and published GPU results.
 *
 * @tparam Backend Render backend. Include that backend's device header before
 *         instantiating this class so @ref RenderDeviceType is specialized.
 */
template <class Backend>
class Renderer {
 public:
  using RenderDevice = typename RenderDeviceType<Backend>::Type;

  explicit Renderer(std::shared_ptr<PipelineDocument> document);
  Renderer(std::shared_ptr<PipelineDocument> document, PreparedSourceCache::UnpackFn unpack);
  ~Renderer();

  Renderer(const Renderer&)                    = delete;
  auto operator=(const Renderer&) -> Renderer& = delete;

  void SetDocument(std::shared_ptr<PipelineDocument> document);

  /**
   * @brief Render one frame on the owning thread.
   *
   * @param cache_policy Session mode reads and publishes reusable editor results. Bypass mode
   *        uses an isolated one-shot workspace and releases its result resources after delivery.
   * @throws std::runtime_error for invalid input, GPU execution, or presentation failure.
   */
  [[nodiscard]] auto Render(const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                            const RenderRequest& request, IFrameSink* sink,
                            const FrameCompletionSubmission& submission, bool require_host_output,
                            RenderCachePolicy cache_policy = RenderCachePolicy::UseSessionCache)
      -> std::shared_ptr<ImageBuffer>;

  /** @brief Snapshot of source and static-plan cache counters since construction or ResetStats. */
  [[nodiscard]] auto Stats() const -> RenderSessionStats;
  void               ResetStats();

  /**
   * @brief Drop GPU result textures, host prepared sources, the plan cache, and
   *        backend session extras. Keeps the session device.
   *
   * Call when the pipeline is returned to PipelineMgmtService. The next Render
   * rebuilds caches from the still-owned PipelineDocument.
   */
  void ReleaseSessionCaches();

  [[nodiscard]] auto SessionResources() const -> RenderSessionResources;

  /**
   * @brief Published result count of the isolated one-shot workspace.
   *
   * Zero when no one-shot device exists or after a successful bypass render that
   * released that workspace. Session caches are not included.
   */
  [[nodiscard]] auto OneShotPublishedResultCount() const -> std::size_t;

  [[nodiscard]] auto Device() -> RenderDevice& { return *device_; }
  [[nodiscard]] auto Device() const -> const RenderDevice& { return *device_; }

  [[nodiscard]] auto SourceCache() -> PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto SourceCache() const -> const PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto PlanCache() -> StaticExecutionPlanCache& { return plan_cache_; }
  [[nodiscard]] auto PlanCache() const -> const StaticExecutionPlanCache& { return plan_cache_; }
  [[nodiscard]] auto MaskAssets() -> MaskStore&;

 private:
  void EnsureOneShotDevice();
  void ConfigureDevice(RenderDevice& device, const char* error_label);

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<RenderDevice>     device_;
  std::unique_ptr<RenderDevice>     one_shot_device_;
  std::unique_ptr<MaskStore>        mask_store_;
  PreparedSourceCache::UnpackFn     unpack_;
  PreparedSourceCache               source_cache_;
  StaticExecutionPlanCache          plan_cache_{Backend::kCapabilityVersion};
};

template <class Backend>
Renderer<Backend>::Renderer(std::shared_ptr<PipelineDocument> document)
    : Renderer(std::move(document), PreparedSourceCache::UnpackFn{}) {}

template <class Backend>
Renderer<Backend>::Renderer(std::shared_ptr<PipelineDocument> document,
                            PreparedSourceCache::UnpackFn     unpack)
    : document_(std::move(document)),
      device_(std::make_unique<RenderDevice>()),
      one_shot_device_(),
      mask_store_(std::make_unique<MaskStore>(std::filesystem::temp_directory_path() /
                                              "alcedo_studio" / "product_mask_store")),
      unpack_(unpack ? std::move(unpack)
                     : PreparedSourceCache::UnpackFn{[](std::span<const std::byte> encoded,
                                                        DecodeRes                  decode_res) {
                         return RawInputLoader::LoadEncoded(encoded, decode_res);
                       }}),
      source_cache_(unpack_),
      plan_cache_(Backend::kCapabilityVersion) {
  ConfigureDevice(*device_, "product");
}

template <class Backend>
Renderer<Backend>::~Renderer() = default;

template <class Backend>
void Renderer<Backend>::ConfigureDevice(RenderDevice& device, const char* error_label) {
  device.Workspace().Textures().SetByteBudget(Backend::DefaultTextureBudgetBytes());
  device.SetErrorReporter([error_label](std::string_view message) {
    std::fprintf(stderr, "[ERROR] %s DAG %s render failed: %.*s\n", Backend::kName, error_label,
                 static_cast<int>(message.size()), message.data());
  });
}

template <class Backend>
void Renderer<Backend>::EnsureOneShotDevice() {
  if (one_shot_device_) {
    return;
  }
  one_shot_device_ = std::make_unique<RenderDevice>();
  ConfigureDevice(*one_shot_device_, "one-shot");
}

template <class Backend>
auto Renderer<Backend>::MaskAssets() -> MaskStore& {
  return *mask_store_;
}

template <class Backend>
void Renderer<Backend>::SetDocument(std::shared_ptr<PipelineDocument> document) {
  if (!document) {
    throw std::invalid_argument("Renderer: PipelineDocument is null");
  }
  document_ = std::move(document);
}

template <class Backend>
auto Renderer<Backend>::Stats() const -> RenderSessionStats {
  const auto         source = source_cache_.GetStats();
  const auto         plan   = plan_cache_.GetStats();
  RenderSessionStats stats;
  stats.prepared_source_hits     = source.hits;
  stats.prepared_source_misses   = source.misses;
  stats.libraw_open_unpack_count = source.libraw_open_unpack_count;
  stats.plan_cache_hits          = plan.hits;
  stats.plan_cache_misses        = plan.misses;
  stats.plan_compile_count       = plan.compiles;
  stats.pass                     = device_->PassStats();
  return stats;
}

template <class Backend>
void Renderer<Backend>::ResetStats() {
  source_cache_.ResetStats();
  plan_cache_.ResetStats();
  device_->ResetPassStats();
}

template <class Backend>
void Renderer<Backend>::ReleaseSessionCaches() {
  if (!device_) {
    return;
  }
  device_->WaitIdle();
  device_->Workspace().ReleaseSessionResources();
  if constexpr (requires(RenderDevice& device) { device.ReleaseNeuralDemosaicWorkspace(); }) {
    device_->ReleaseNeuralDemosaicWorkspace();
  }
  one_shot_device_.reset();
  source_cache_.Clear();
  plan_cache_.Clear();
  device_->ResetPassStats();
}

template <class Backend>
auto Renderer<Backend>::SessionResources() const -> RenderSessionResources {
  RenderSessionResources resources;
  if (!device_) {
    return resources;
  }
  const auto& workspace                 = device_->Workspace();
  resources.published_result_count      = workspace.Images().PublishedCount();
  resources.texture_pool_used_bytes     = workspace.Textures().UsedBytes();
  resources.texture_pool_entry_count    = workspace.Textures().EntryCount();
  resources.prepared_source_host_bytes  = source_cache_.HostBytesUsed();
  resources.prepared_source_entry_count = source_cache_.EntryCount();
  resources.session_value_ids           = workspace.Images().CurrentValueIds();
  return resources;
}

template <class Backend>
auto Renderer<Backend>::OneShotPublishedResultCount() const -> std::size_t {
  if (!one_shot_device_) {
    return 0;
  }
  return one_shot_device_->Workspace().Images().PublishedCount();
}

}  // namespace alcedo
