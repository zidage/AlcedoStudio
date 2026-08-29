//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_render_device.hpp"

#include <cuda_runtime.h>

#include "cuda_drt_runtime_state.cuh"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"

namespace alcedo {

CudaRenderDevice::CudaRenderDevice() : drt_runtime_(std::make_unique<CudaDrtRuntimeState>()) {}

CudaRenderDevice::~CudaRenderDevice() {
  try {
    WaitIdle();
  } catch (...) {
  }
}

void CudaRenderDevice::CancelRender() noexcept {
  if (!workspace_.IsRendering()) return;
  (void)::cudaStreamSynchronize(command_context_.Stream());
  workspace_.CancelRender();
}

auto CudaRenderDevice::DrtRuntime() -> CudaDrtRuntimeState& { return *drt_runtime_; }

auto CudaRenderDevice::NeuralDemosaicWorkspace() -> CUDA::NeuralDemosaicWorkspace& {
  if (!neural_workspace_) {
    neural_workspace_ = std::make_unique<CUDA::NeuralDemosaicWorkspace>();
  }
  return *neural_workspace_;
}

void CudaRenderDevice::ReleaseNeuralDemosaicWorkspace() { neural_workspace_.reset(); }

}  // namespace alcedo
