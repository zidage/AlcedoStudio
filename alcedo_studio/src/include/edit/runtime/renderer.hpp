//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/input/raster_input_loader.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/render_device_type.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "edit/runtime/static_execution_plan_cache.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class ImageBuffer;

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
 * @brief Live GPU/host allocations owned by one renderer's device and source cache.
 */
struct RenderSessionResources {
  std::size_t               published_result_count      = 0;
  std::size_t               texture_pool_used_bytes     = 0;
  std::size_t               texture_pool_entry_count    = 0;
  std::size_t               prepared_source_host_bytes  = 0;
  std::size_t               prepared_source_entry_count = 0;
  std::size_t               transient_used_bytes        = 0;
  std::size_t               transient_capacity_bytes    = 0;
  std::size_t               transient_slab_count        = 0;
  std::vector<GraphValueId> session_value_ids;
};

using CudaProductSessionResources = RenderSessionResources;

/**
 * @brief GPU DAG renderer of one backend in one @ref ExecutorRole.
 *
 * Holds no document. Every render receives an immutable @ref PipelineGraphSnapshot and only
 * reads it; parameter changes are found by revision, so any number of renderers can read the
 * same snapshot. The role is fixed at construction:
 *
 * - Interactive: keeps prepared sources, compiled static plans, and published GPU results for
 *   the current @ref RenderBindingKey. A render with another key first releases all of them
 *   (@ref ReleaseBinding). Frames may present to a sink.
 * - Batch: the device uses a dedicated submission queue. Every render unpacks its source and
 *   compiles its plan, and releases every result resource after delivery or failure.
 *
 * Thread: one render at a time, on the caller's thread (the executor's render lock).
 *
 * @tparam Backend Render backend. Include that backend's device header before
 *         instantiating this class so @ref RenderDeviceType is specialized.
 */
template <class Backend>
class Renderer {
 public:
  using RenderDevice = typename RenderDeviceType<Backend>::Type;

  /**
   * @param lut_resources Resolver for Color Grade LUT references, shared with the device
   *        workspace. Null selects the file-path-only DefaultLutResourceResolver.
   */
  explicit Renderer(ExecutorRole role, PreparedSourceCache::UnpackFn unpack = {},
                    std::shared_ptr<const LutResourceResolver> lut_resources = nullptr);
  ~Renderer();

  Renderer(const Renderer&)                    = delete;
  auto operator=(const Renderer&) -> Renderer& = delete;

  [[nodiscard]] auto Role() const -> ExecutorRole { return role_; }

  /**
   * @brief Render one frame of @p snapshot.
   *
   * An interactive renderer whose binding differs from the snapshot's key releases the previous
   * binding first. A batch renderer releases its result resources before it returns or throws.
   *
   * @pre No other render runs on this renderer.
   * @throws std::invalid_argument when `request.role` differs from @ref Role.
   * @throws std::runtime_error for invalid input, GPU execution, or presentation failure.
   */
  [[nodiscard]] auto Render(const PipelineGraphSnapshot&        snapshot,
                            const std::shared_ptr<ImageBuffer>& input,
                            const PipelineApplyRequest& request) -> std::shared_ptr<ImageBuffer>;

  /**
   * @brief Render one frame of @p snapshot like @ref Render, and also return the exact geometry
   *        and display encoding of the executed plan.
   *
   * @ref Render returns the `pixels` member of this result. `pixels` holds the host download
   * when `request.require_host_output` is true, and an empty buffer otherwise. An interactive
   * request with frame role QualityBase keeps only the sensor result and releases every
   * downstream result of this render after presentation and download.
   *
   * @pre No other render runs on this renderer.
   * @throws std::invalid_argument when `request.role` differs from @ref Role.
   * @throws std::runtime_error for invalid input, GPU execution, presentation, or download
   *         failure. Nothing is returned for a failed render.
   */
  [[nodiscard]] auto RenderImage(const PipelineGraphSnapshot&        snapshot,
                                 const std::shared_ptr<ImageBuffer>& input,
                                 const PipelineApplyRequest& request) -> RenderedPipelineImage;

  /**
   * @brief Render one frame from explicit arguments; the request role is this renderer's role.
   */
  [[nodiscard]] auto Render(const PipelineGraphSnapshot&        snapshot,
                            const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                            const RenderRequest& request, IFrameSink* sink,
                            const FrameCompletionSubmission& submission, bool require_host_output,
                            const std::optional<ExportColorProfileConfig>& output_color = {})
      -> std::shared_ptr<ImageBuffer>;

  /** @brief Source and static-plan cache counters since construction or ResetStats. */
  [[nodiscard]] auto Stats() const -> RenderSessionStats;
  void               ResetStats();

  /**
   * @brief Release every resource of the current binding and forget the binding.
   *
   * Drops published GPU results, transients, local tone caches, the plan cache, prepared
   * sources, and the neural demosaic workspace after the device is idle. The device and its
   * queue are kept, so the next render does not create GPU streams again.
   * @pre No render runs on this renderer.
   */
  void ReleaseBinding();

  /// Binding of the last interactive render; empty for a batch renderer and after a release.
  [[nodiscard]] auto Binding() const -> const std::optional<RenderBindingKey>& { return binding_; }

  /// Live allocations of this renderer's device and source cache.
  [[nodiscard]] auto Resources() const -> RenderSessionResources;

  /**
   * @brief Address of the device, or 0 when none exists. Stable until the renderer is
   *        destroyed; tests use it to detect device reconstruction.
   */
  [[nodiscard]] auto DebugDeviceIdentity() const -> std::uintptr_t;

  /**
   * @brief Native queue/stream identity of the device, or 0 when none exists or the backend
   *        has no queue query. Tests use it to verify that batch renderers do not share a queue.
   */
  [[nodiscard]] auto DebugQueueIdentity() const -> std::uintptr_t;

  [[nodiscard]] auto Device() -> RenderDevice& {
    EnsureDevice();
    return *device_;
  }
  [[nodiscard]] auto Device() const -> const RenderDevice& {
    if (!device_) {
      throw std::runtime_error("Renderer: device has not been created");
    }
    return *device_;
  }

  [[nodiscard]] auto SourceCache() -> PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto SourceCache() const -> const PreparedSourceCache& { return source_cache_; }
  [[nodiscard]] auto PlanCache() -> StaticExecutionPlanCache& { return plan_cache_; }
  [[nodiscard]] auto PlanCache() const -> const StaticExecutionPlanCache& { return plan_cache_; }

 private:
  void EnsureDevice();
  void ReleaseDeviceResources();

  ExecutorRole                    role_;
  std::unique_ptr<RenderDevice>   device_;
  PreparedSourceCache::UnpackFn   unpack_;
  PreparedSourceCache             source_cache_;
  StaticExecutionPlanCache        plan_cache_{Backend::kCapabilityVersion};
  std::optional<RenderBindingKey> binding_;
  std::shared_ptr<const LutResourceResolver> lut_resources_;
};

template <class Backend>
Renderer<Backend>::Renderer(ExecutorRole role, PreparedSourceCache::UnpackFn unpack,
                            std::shared_ptr<const LutResourceResolver> lut_resources)
    : role_(role),
      device_(),
      unpack_(unpack ? std::move(unpack)
                     : PreparedSourceCache::UnpackFn{[](std::span<const std::byte> encoded,
                                                        DecodeRes                  decode_res) {
                         return LoadEncodedImage(encoded, decode_res);
                       }}),
      source_cache_(unpack_),
      plan_cache_(Backend::kCapabilityVersion),
      lut_resources_(lut_resources ? std::move(lut_resources) : DefaultLutResourceResolver()) {}

template <class Backend>
Renderer<Backend>::~Renderer() = default;

template <class Backend>
void Renderer<Backend>::EnsureDevice() {
  if (device_) {
    return;
  }
  device_ = std::make_unique<RenderDevice>();
  device_->Workspace().SetLutResources(lut_resources_);
  if (role_ == ExecutorRole::Batch) {
    if constexpr (requires {
                    { device_->Workspace().Device().UseDedicatedQueue() };
                  }) {
      // Batch devices get an isolated submission stream (CUDA uses a stream per command
      // context; OpenCL a dedicated command queue) so parallel batch renders never share one
      // queue object.
      device_->Workspace().Device().UseDedicatedQueue();
    }
  }
  const char* label = role_ == ExecutorRole::Batch ? "batch" : "interactive";
  device_->SetErrorReporter([label](std::string_view message) {
    std::fprintf(stderr, "[ERROR] %s DAG %s render failed: %.*s\n", Backend::kName, label,
                 static_cast<int>(message.size()), message.data());
  });
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
  if (device_) {
    stats.pass = device_->PassStats();
  }
  return stats;
}

template <class Backend>
void Renderer<Backend>::ResetStats() {
  source_cache_.ResetStats();
  plan_cache_.ResetStats();
  if (device_) {
    device_->ResetPassStats();
  }
}

template <class Backend>
void Renderer<Backend>::ReleaseDeviceResources() {
  if (!device_) {
    return;
  }
  device_->WaitIdle();
  device_->Workspace().ReleaseSessionResources();
  if constexpr (requires(RenderDevice& device) { device.ReleaseNeuralDemosaicWorkspace(); }) {
    device_->ReleaseNeuralDemosaicWorkspace();
  }
}

template <class Backend>
void Renderer<Backend>::ReleaseBinding() {
  source_cache_.Clear();
  plan_cache_.Clear();
  binding_.reset();
  ReleaseDeviceResources();
  if (device_) {
    device_->ResetPassStats();
  }
}

template <class Backend>
auto Renderer<Backend>::Resources() const -> RenderSessionResources {
  RenderSessionResources resources;
  resources.prepared_source_host_bytes  = source_cache_.HostBytesUsed();
  resources.prepared_source_entry_count = source_cache_.EntryCount();
  if (!device_) {
    return resources;
  }
  const auto& workspace              = device_->Workspace();
  resources.published_result_count   = workspace.Images().PublishedCount();
  resources.texture_pool_used_bytes  = workspace.Textures().UsedBytes();
  resources.texture_pool_entry_count = workspace.Textures().EntryCount();
  resources.transient_used_bytes     = workspace.TransientBuffers().used_bytes();
  resources.transient_capacity_bytes = workspace.TransientBuffers().capacity_bytes();
  resources.transient_slab_count     = workspace.TransientBuffers().slab_count();
  resources.session_value_ids        = workspace.Images().CurrentValueIds();
  return resources;
}

template <class Backend>
auto Renderer<Backend>::DebugDeviceIdentity() const -> std::uintptr_t {
  return reinterpret_cast<std::uintptr_t>(device_.get());
}

template <class Backend>
auto Renderer<Backend>::DebugQueueIdentity() const -> std::uintptr_t {
  std::uintptr_t identity = 0;
  if (device_) {
    if constexpr (requires {
                    { device_->Workspace().Device().NativeQueue() };
                  }) {
      identity = reinterpret_cast<std::uintptr_t>(device_->Workspace().Device().NativeQueue());
    }
  }
  return identity;
}

}  // namespace alcedo
