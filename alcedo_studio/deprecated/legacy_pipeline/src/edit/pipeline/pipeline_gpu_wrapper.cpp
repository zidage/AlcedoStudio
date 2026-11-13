//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/pipeline/pipeline_gpu_wrapper.hpp"

#include <stdexcept>
#include <utility>

#include "edit/pipeline/pipeline_accelerator.hpp"

namespace alcedo {
#ifdef HAVE_CUDA
auto CreateCUDAGPUPipeline() -> std::unique_ptr<GPUPipelineImpl>;
#endif
#ifdef HAVE_METAL
auto CreateMetalGPUPipeline() -> std::unique_ptr<GPUPipelineImpl>;
#endif
#ifdef HAVE_OPENCL
auto CreateOpenCLGPUPipeline() -> std::unique_ptr<GPUPipelineImpl>;
#endif

namespace {
class UnavailableGPUPipeline final : public GPUPipelineImpl {
 public:
  void SetInputImage(std::shared_ptr<ImageBuffer>) override {}

  void SetParams(OperatorParams&) override {}

  void SetFrameSink(IFrameSink*) override {}

  void SetBoundFrameSubmission(const FrameCompletionSubmission&) override {}

  void Execute(std::shared_ptr<ImageBuffer>) override {
    throw std::runtime_error("GPU backend unavailable: compiled GPU pipeline implementation is missing.");
  }

  void ReleaseScratchBuffers() override {}

  void ReleaseResources() override {}
};

auto CreateGPUPipeline(const GpuBackendKind backend) -> std::unique_ptr<GPUPipelineImpl> {
  switch (backend) {
    case GpuBackendKind::CUDA:
#ifdef HAVE_CUDA
      return CreateCUDAGPUPipeline();
#else
      break;
#endif
    case GpuBackendKind::Metal:
#ifdef HAVE_METAL
      return CreateMetalGPUPipeline();
#else
      break;
#endif
    case GpuBackendKind::OpenCL:
#ifdef HAVE_OPENCL
      return CreateOpenCLGPUPipeline();
#else
      break;
#endif
    case GpuBackendKind::None:
      break;
  }
  return std::make_unique<UnavailableGPUPipeline>();
}
}  // namespace

GPUPipelineWrapper::GPUPipelineWrapper() : GPUPipelineWrapper(GpuBackendKind::None) {}

GPUPipelineWrapper::GPUPipelineWrapper(GpuBackendKind backend)
    : impl_(CreateGPUPipeline(backend)), backend_(backend) {}

GPUPipelineWrapper::~GPUPipelineWrapper() {
  if (impl_) {
    impl_->ReleaseResources();
  }
}

void GPUPipelineWrapper::SetBackend(const GpuBackendKind backend) {
  if (backend_ == backend && impl_) {
    return;
  }
  if (impl_) {
    impl_->ReleaseResources();
  }
  backend_ = backend;
  impl_    = CreateGPUPipeline(backend_);
}

auto GPUPipelineWrapper::Backend() const -> GpuBackendKind { return backend_; }

void GPUPipelineWrapper::SetInputImage(std::shared_ptr<ImageBuffer> input_image) {
  impl_->SetInputImage(std::move(input_image));
}

void GPUPipelineWrapper::SetParams(OperatorParams& params) { impl_->SetParams(params); }

void GPUPipelineWrapper::SetFrameSink(IFrameSink* frame_sink) { impl_->SetFrameSink(frame_sink); }

void GPUPipelineWrapper::SetBoundFrameSubmission(const FrameCompletionSubmission& submission) {
  impl_->SetBoundFrameSubmission(submission);
}

void GPUPipelineWrapper::Execute(std::shared_ptr<ImageBuffer> output) {
  impl_->Execute(std::move(output));
}

auto GPUPipelineWrapper::HasAcceleratedBackend() const -> bool {
  return IsImplementedMergedPipelineBackend(backend_);
}

void GPUPipelineWrapper::ReleaseResources() { impl_->ReleaseResources(); }

void GPUPipelineWrapper::ReleaseScratchBuffers() { impl_->ReleaseScratchBuffers(); }

auto GPUPipelineWrapper::DebugGetAllocatedScratchBytes() const -> size_t {
  return impl_->DebugGetAllocatedScratchBytes();
}
}  // namespace alcedo
