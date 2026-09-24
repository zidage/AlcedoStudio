//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/GPU_kernels/basic.cuh"
#include "edit/operators/GPU_kernels/color.cuh"
#include "edit/operators/GPU_kernels/cst.cuh"
#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/pipeline/cuda_output_texture_tail.cuh"
#include "edit/pipeline/gpu_scheduler.cuh"
#include "edit/pipeline/kernel_stream_gpu.cuh"
#include "edit/pipeline/pipeline_gpu_wrapper.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
using namespace CUDA;

class CUDA_GPUPipeline final : public GPUPipelineImpl {
 private:
  static constexpr auto BuildKernelStream = []() {
    auto to_ws  = GPU_TOWS_Kernel();
    auto exp    = GPU_ExposureOpKernel();
    auto cont   = GPU_ContrastOpKernel();
    auto tone   = GPU_ToneOpKernel();
    auto hs     = GPU_HighlightShadowLocalToneStage();
    auto curve  = GPU_CurveOpKernel();

    auto vib    = GPU_VibranceOpKernel();
    auto wheel  = GPU_ColorWheelOpKernel();
    auto hls    = GPU_HLSOpKernel();
    auto lmt    = GPU_LMT_Kernel();
    auto to_out = GPU_OUTPUT_Kernel();

    return MakeCudaPipelineKernelStream(GPU_PointChain(to_ws, exp, cont, tone), hs,
                                        GPU_PointChain(curve, vib, wheel, hls, lmt, to_out));
  };

  using StaticKernelStreamType                                     = decltype(BuildKernelStream());
  StaticKernelStreamType                     static_kernel_stream_ = BuildKernelStream();
  GPU_KernelLauncher<StaticKernelStreamType> launcher_;

 public:
  CUDA_GPUPipeline() : launcher_(nullptr, static_kernel_stream_) {}

  void SetInputImage(std::shared_ptr<ImageBuffer> input_img) override {
    launcher_.SetInputImage(std::move(input_img));
  }

  void SetParams(OperatorParams& cpu_params) override { launcher_.SetParams(cpu_params); }

  void SetFrameSink(IFrameSink* frame_sink) override { launcher_.SetFrameSink(frame_sink); }

  void SetBoundFrameSubmission(const FrameCompletionSubmission& submission) override {
    launcher_.SetBoundFrameSubmission(submission);
  }

  void Execute(std::shared_ptr<ImageBuffer> output_img) override {
    launcher_.SetOutputImage(output_img);
    launcher_.Execute();
  }

  void               ReleaseScratchBuffers() override { launcher_.ReleaseScratchBuffers(); }

  void               ReleaseResources() override { launcher_.ReleaseResources(); }

  [[nodiscard]] auto DebugGetAllocatedScratchBytes() const -> size_t override {
    return launcher_.GetAllocatedScratchBytes();
  }
};

auto CreateCUDAGPUPipeline() -> std::unique_ptr<GPUPipelineImpl> {
  return std::make_unique<CUDA_GPUPipeline>();
}
}  // namespace alcedo
