//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/render_device_type.hpp"
#include "gpu/transient_allocation_policy.hpp"

namespace alcedo {

using CudaRenderWorkspace = BasicRenderWorkspace<CudaBackend>;

class CudaDrtRuntimeState;
class MaskStore;

namespace CUDA {
class NeuralDemosaicWorkspace;
}

/**
 * @brief Owns a CUDA workspace and one command context. Does not own a graph or plan.
 *
 * Device loss is the only reason to destroy this object. Not thread-safe.
 */
class CudaRenderDevice {
 public:
  CudaRenderDevice();
  ~CudaRenderDevice();

  CudaRenderDevice(const CudaRenderDevice&)                                  = delete;
  auto               operator=(const CudaRenderDevice&) -> CudaRenderDevice& = delete;

  [[nodiscard]] auto Workspace() -> CudaRenderWorkspace& { return workspace_; }
  [[nodiscard]] auto Workspace() const -> const CudaRenderWorkspace& { return workspace_; }
  [[nodiscard]] auto CommandContext() -> CudaCommandContext& { return command_context_; }

  void               BeginRender() { workspace_.BeginRender(command_context_); }
  void               EndRender() { workspace_.EndRender(command_context_); }
  void               WaitIdle() { workspace_.Device().Wait(command_context_); }
  void               CancelRender() noexcept;

  /**
   * @brief Publish unpublished GPU image results for the submission ended by EndRender.
   *
   * Call after a successful present or host download. Kernel or frame-sink failure must
   * leave results unpublished via CancelRender / DiscardUnpublished.
   */
  void PublishResults() {
    workspace_.Images().PublishSuccessfulSubmission(command_context_.SubmissionId());
  }

  [[nodiscard]] auto PassStats() -> GpuNodePassStats& { return pass_stats_; }
  [[nodiscard]] auto PassStats() const -> const GpuNodePassStats& { return pass_stats_; }
  void               ResetPassStats() { pass_stats_.Reset(); }

  /** @brief Install the app-layer error receiver. Called synchronously on the render thread. */
  void               SetErrorReporter(std::function<void(std::string_view)> reporter) {
    error_reporter_ = std::move(reporter);
  }
  void ReportError(std::string_view message) const {
    if (error_reporter_) error_reporter_(message);
  }

  [[nodiscard]] auto DrtRuntime() -> CudaDrtRuntimeState&;

  /**
   * @brief Neural Engine tile activation workspace. Created on first Neural develop
   *        in this render; released with develop scratch after SensorDevelop.
   *        Weights stay in the process cache.
   */
  [[nodiscard]] auto NeuralDemosaicWorkspace() -> CUDA::NeuralDemosaicWorkspace&;

/** @brief Drop Neural Engine tile activations. Weights stay in the process cache. */
  void ReleaseNeuralDemosaicWorkspace();

  /**
   * @brief Execute the complete compiled CUDA DAG and return its display texture identity.
   *
   * Skips GPU node passes whose content keys are already published. When
   * @p publish_on_success is true, unpublished writes are published after a successful
   * submit. Product present paths pass false and call @ref PublishResults after the sink
   * succeeds. Reports and rethrows failures after cancelling the incomplete submission.
   * There is no CPU image-processing fallback.
   */
  [[nodiscard]] auto Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                             PipelineDocument& document, MaskStore* mask_store = nullptr,
                             bool publish_on_success = true,
                             TransientAllocationPolicy transient_policy =
                                 TransientAllocationPolicy::SessionPacked,
                             std::span<const ActiveRasterMaskInput> active_raster_masks = {})
      -> GraphValueId;

 private:
  CudaRenderWorkspace                              workspace_;
  CudaCommandContext                               command_context_;
  std::unique_ptr<CudaDrtRuntimeState>             drt_runtime_;
  std::unique_ptr<CUDA::NeuralDemosaicWorkspace>   neural_workspace_;
  std::function<void(std::string_view)>            error_reporter_;
  GpuNodePassStats                                 pass_stats_{};
};

template <>
struct RenderDeviceType<CudaBackend> {
  using Type = CudaRenderDevice;
};

}  // namespace alcedo
