//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"

namespace alcedo {

using CudaRenderWorkspace = BasicRenderWorkspace<CudaBackend>;

class CudaDrtRuntimeState;
class MaskStore;
class PipelineDocument;
struct ExecutionPlan;
struct PreparedRawInput;

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

  /** @brief Install the app-layer error receiver. Called synchronously on the render thread. */
  void               SetErrorReporter(std::function<void(std::string_view)> reporter) {
    error_reporter_ = std::move(reporter);
  }
  void ReportError(std::string_view message) const {
    if (error_reporter_) error_reporter_(message);
  }

  [[nodiscard]] auto DrtRuntime() -> CudaDrtRuntimeState&;

  /**
   * @brief Execute the complete compiled CUDA DAG and return its display texture identity.
   *
   * Reports and rethrows failures after cancelling the incomplete submission. There is no CPU
   * image-processing fallback.
   */
  [[nodiscard]] auto Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                             PipelineDocument& document, MaskStore* mask_store = nullptr)
      -> GraphValueId;

 private:
  CudaRenderWorkspace                   workspace_;
  CudaCommandContext                    command_context_;
  std::unique_ptr<CudaDrtRuntimeState>  drt_runtime_;
  std::function<void(std::string_view)> error_reporter_;
};

}  // namespace alcedo
