//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <span>
#include <string_view>
#include <variant>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/plan_executor.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "gpu/transient_allocation_policy.hpp"

namespace alcedo {

/**
 * @brief Workspace, command context, pass stats, and plan execution for one backend.
 *
 * Device loss is the only reason to destroy this object. Not thread-safe.
 * CUDA adds Neural and DRT session objects on @ref CudaRenderDevice.
 *
 * @tparam Backend Render backend resource factory.
 */
template <class Backend>
class BasicRenderDevice {
 public:
  using WorkspaceType      = BasicRenderWorkspace<Backend>;
  using CommandContextType = typename Backend::CommandContext;

  BasicRenderDevice() = default;
  ~BasicRenderDevice() {
    try {
      WaitIdle();
    } catch (...) {
    }
  }

  BasicRenderDevice(const BasicRenderDevice&)                    = delete;
  auto operator=(const BasicRenderDevice&) -> BasicRenderDevice& = delete;

  [[nodiscard]] auto Workspace() -> WorkspaceType& { return workspace_; }
  [[nodiscard]] auto Workspace() const -> const WorkspaceType& { return workspace_; }
  [[nodiscard]] auto CommandContext() -> CommandContextType& { return command_context_; }

  void               BeginRender() { workspace_.BeginRender(command_context_); }
  void               EndRender() { workspace_.EndRender(command_context_); }
  /** @brief Wait for submitted work, then release textures a published Develop rewrite replaced. */
  void               WaitIdle() {
    workspace_.Device().Wait(command_context_);
    workspace_.ReleaseTexturesReplacedByDevelopRewrite();
  }

  void BeginGpuWorkSample() {
    if constexpr (requires(Backend& backend, CommandContextType& context) {
                    backend.BeginGpuWorkSample(context);
                  }) {
      workspace_.Device().BeginGpuWorkSample(command_context_);
    }
  }
  void EndGpuWorkSample() {
    if constexpr (requires(Backend& backend, CommandContextType& context) {
                    backend.EndGpuWorkSample(context);
                  }) {
      workspace_.Device().EndGpuWorkSample(command_context_);
    }
  }
  void ResolveGpuTimestamps() {
    if constexpr (requires(Backend& backend) { backend.ResolveGpuTimestamps(); }) {
      workspace_.Device().ResolveGpuTimestamps();
    }
  }

  /** @brief Release backend-owned neural activation workspace after GPU completion. */
  void ReleaseNeuralDemosaicWorkspace() {
    if constexpr (requires(Backend& backend) { backend.ReleaseNeuralDemosaicWorkspace(); }) {
      workspace_.Device().ReleaseNeuralDemosaicWorkspace();
    }
  }

  /**
   * @brief Drop unpublished writes for a failed encode. Does not publish results.
   */
  void CancelRender() noexcept {
    if (!workspace_.IsRendering()) {
      return;
    }
    bool wait_succeeded = false;
    try {
      workspace_.Device().Wait(command_context_);
      wait_succeeded = true;
    } catch (...) {
    }
    if (wait_succeeded) {
      if constexpr (requires(Backend& backend) { backend.ReleaseUnsubmittedResourceUses(); }) {
        workspace_.Device().ReleaseUnsubmittedResourceUses();
      }
    }
    workspace_.CancelRender();
  }

  /**
   * @brief Publish unpublished GPU image results for the submission ended by EndRender.
   *
   * Call after a successful present or host download. Kernel or frame-sink failure must
   * leave results unpublished via CancelRender.
   */
  void PublishResults() {
    workspace_.PublishImageResults(command_context_.SubmissionId());
  }

  [[nodiscard]] auto PassStats() -> GpuNodePassStats& { return pass_stats_; }
  [[nodiscard]] auto PassStats() const -> const GpuNodePassStats& { return pass_stats_; }
  void               ResetPassStats() { pass_stats_.Reset(); }

  /** @brief Install the app-layer error receiver. Called synchronously on the render thread. */
  void SetErrorReporter(std::function<void(std::string_view)> reporter) {
    error_reporter_ = std::move(reporter);
  }
  void ReportError(std::string_view message) const {
    if (error_reporter_) {
      error_reporter_(message);
    }
  }

  /**
   * @brief Run the compiled DAG. Skips published content keys. No image-processing substitute.
   */
  [[nodiscard]] auto Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                             PipelineDocument& document, bool publish_on_success = true,
                             TransientAllocationPolicy transient_policy =
                                 TransientAllocationPolicy::SessionPacked,
                             ResultPersistenceScope persistence =
                                 ResultPersistenceScope::AllCurrentResults)
      -> GraphValueId {
    // Backends with an isolated submission stream (OpenCL dedicated queue)
    // bind it to this thread for the encode so shared helpers that resolve a
    // queue through the global context land on the render-local stream.
    auto queue_scope = EnterThreadQueueScope();
    return PlanExecutor<Backend>::Execute(*this, plan, input, document, publish_on_success,
                                          transient_policy, persistence);
  }

 private:
  auto EnterThreadQueueScope() {
    if constexpr (requires(Backend& backend) { backend.BindThreadQueue(); }) {
      return workspace_.Device().BindThreadQueue();
    } else {
      return std::monostate{};
    }
  }

  WorkspaceType                         workspace_;
  CommandContextType                    command_context_;
  GpuNodePassStats                      pass_stats_{};
  std::function<void(std::string_view)> error_reporter_;
};

}  // namespace alcedo
