//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"

namespace alcedo {

using CudaRenderWorkspace = BasicRenderWorkspace<CudaBackend>;

/**
 * @brief Owns a CUDA workspace and one command context. Does not own a graph or plan.
 *
 * Device loss is the only reason to destroy this object. Not thread-safe.
 */
class CudaRenderDevice {
 public:
  [[nodiscard]] auto Workspace() -> CudaRenderWorkspace& { return workspace_; }
  [[nodiscard]] auto Workspace() const -> const CudaRenderWorkspace& { return workspace_; }
  [[nodiscard]] auto CommandContext() -> CudaCommandContext& { return command_context_; }

  void BeginRender() { workspace_.BeginRender(command_context_); }
  void EndRender() { workspace_.EndRender(command_context_); }
  void WaitIdle() { workspace_.Device().Wait(command_context_); }

  ~CudaRenderDevice() {
    try {
      WaitIdle();
    } catch (...) {
    }
  }

 private:
  CudaRenderWorkspace workspace_;
  CudaCommandContext  command_context_;
};

}  // namespace alcedo
