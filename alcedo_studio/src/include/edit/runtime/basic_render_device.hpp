//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <string_view>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/gpu_node_pass_stats.hpp"
#include "edit/runtime/plan_executor.hpp"

namespace alcedo {

class MaskStore;

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
  void               WaitIdle() { workspace_.Device().Wait(command_context_); }

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
    workspace_.Images().PublishSuccessfulSubmission(command_context_.SubmissionId());
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
                             PipelineDocument& document, MaskStore* mask_store = nullptr,
                             bool publish_on_success = true) -> GraphValueId {
    return PlanExecutor<Backend>::Execute(*this, plan, input, document, mask_store,
                                          publish_on_success);
  }

 private:
  WorkspaceType                         workspace_;
  CommandContextType                    command_context_;
  GpuNodePassStats                      pass_stats_{};
  std::function<void(std::string_view)> error_reporter_;
};

}  // namespace alcedo
