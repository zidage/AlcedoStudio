//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"

#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "decoders/processor/neural_tile_jobs.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "opencl/nn/activation_slots.hpp"
#include "opencl/nn/common.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/nn/demosaicnet_stage_profiler.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo {
namespace {

class KernelHolder {
 public:
  KernelHolder() = default;
  ~KernelHolder() { Reset(); }
  KernelHolder(const KernelHolder&)                    = delete;
  auto operator=(const KernelHolder&) -> KernelHolder& = delete;
  KernelHolder(KernelHolder&& other) noexcept : kernel_(other.kernel_) { other.kernel_ = nullptr; }
  auto operator=(KernelHolder&& other) noexcept -> KernelHolder& {
    if (this != &other) {
      Reset();
      kernel_       = other.kernel_;
      other.kernel_ = nullptr;
    }
    return *this;
  }

  void Create(cl_program program, const char* name) {
    Reset();
    cl_int err = CL_SUCCESS;
    kernel_    = clCreateKernel(program, name, &err);
    opencl::nn::CheckOpenCl(err, name);
    NoteOpenClCreateKernel();
  }
  [[nodiscard]] auto get() const -> cl_kernel { return kernel_; }
  [[nodiscard]] auto empty() const -> bool { return kernel_ == nullptr; }

 private:
  void Reset() noexcept {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
      NoteOpenClReleaseKernel();
      kernel_ = nullptr;
    }
  }
  cl_kernel kernel_ = nullptr;
};

template <typename T>
void SetArg(cl_kernel kernel, cl_uint index, const T& value, const char* what) {
  opencl::nn::CheckOpenCl(clSetKernelArg(kernel, index, sizeof(T), &value), what);
}

void Enqueue2D(cl_kernel kernel, int width, int height, cl_command_queue queue, const char* what) {
  const size_t                 global[] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  opencl::nn::ScopedStageEvent stage_event;
  opencl::nn::CheckOpenCl(clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, nullptr, 0,
                                                 nullptr, stage_event.out()),
                          what);
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
  NoteOpenClEnqueueNdRange();
}

auto ResolveQueue(cl_command_queue queue) -> cl_command_queue {
  if (queue != nullptr) {
    return queue;
  }
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    throw std::runtime_error("OpenClDemosaicNetTiledExecutor: OpenCL context not initialized");
  }
  return context.Queue();
}

template <typename Module>
auto TileOutputBytes() -> std::size_t {
  return static_cast<std::size_t>(Module::kTileOutput) * Module::kTileOutput * 3 * sizeof(float);
}

}  // namespace

struct OpenClDemosaicNetTiledExecutor::Impl {
  KernelHolder             assemble;
  opencl::nn::DeviceBuffer tile_output;

  void                     EnsureKernels() {
    if (!assemble.empty()) {
      return;
    }
    RegisterOpenClBackendPrograms();
    const cl_program program =
        OpenClProgramLibrary::Instance().GetProgram(OpenCL::DemosaicNet::kStructuralProgramName);
    assemble.Create(program, OpenCL::DemosaicNet::kAssembleRgbTileKernelName);
  }

  void EnsureTileBuffers(std::size_t output_bytes) {
    if (tile_output.byte_capacity() < output_bytes) {
      tile_output = opencl::nn::DeviceBuffer(output_bytes);
    }
  }
};

OpenClDemosaicNetTiledExecutor::OpenClDemosaicNetTiledExecutor()
    : impl_(std::make_unique<Impl>()) {}
OpenClDemosaicNetTiledExecutor::~OpenClDemosaicNetTiledExecutor() = default;
OpenClDemosaicNetTiledExecutor::OpenClDemosaicNetTiledExecutor(
    OpenClDemosaicNetTiledExecutor&&) noexcept = default;
auto OpenClDemosaicNetTiledExecutor::operator=(OpenClDemosaicNetTiledExecutor&&) noexcept
    -> OpenClDemosaicNetTiledExecutor& = default;

namespace {

template <typename Module>
auto EnqueueTiles(OpenClDemosaicNetTiledExecutor::Impl& state, const Module& module,
                  opencl::nn::ActivationSlots&          activation_slots,
                  const OpenClDemosaicNetTiledDispatch& dispatch,
                  const detail::NeuralTilePolicy&       policy) -> OpenClDemosaicNetTiledResult {
  if (!module.weights_loaded()) {
    throw std::runtime_error("OpenClDemosaicNetTiledExecutor: module weights are not loaded");
  }
  if (dispatch.input_aligned_hwc == nullptr || dispatch.output_aligned_hwc == nullptr ||
      dispatch.aligned_width <= 0 || dispatch.aligned_height <= 0) {
    throw std::runtime_error("OpenClDemosaicNetTiledExecutor: invalid aligned input or output");
  }
  if ((dispatch.aligned_width % Module::kCfaPeriod) != 0 ||
      (dispatch.aligned_height % Module::kCfaPeriod) != 0) {
    throw std::runtime_error(
        "OpenClDemosaicNetTiledExecutor: aligned dimensions violate CFA period");
  }

  const cl_command_queue queue = ResolveQueue(dispatch.queue);
  state.EnsureKernels();
  state.EnsureTileBuffers(TileOutputBytes<Module>());

  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, dispatch.aligned_width, dispatch.aligned_height),
                            cv::Size(dispatch.aligned_width, dispatch.aligned_height), policy);

  // The in-order queue makes one pair of staging buffers safe: every pack,
  // forward, and assembly completes before the next job overwrites either one.
  // There is deliberately no clFinish/event wait in this loop.
  for (const auto& job : jobs) {
    if (job.input_w != Module::kTileInput || job.input_h != Module::kTileInput ||
        job.owned_w != Module::kTileOutput || job.owned_h != Module::kTileOutput ||
        (job.input_origin.x % Module::kCfaPeriod) != 0 ||
        (job.input_origin.y % Module::kCfaPeriod) != 0) {
      throw std::runtime_error("OpenClDemosaicNetTiledExecutor: invalid shared tile job geometry");
    }

    const cl_int frame_h  = dispatch.aligned_height;
    const cl_int frame_w  = dispatch.aligned_width;
    const cl_int origin_y = job.input_origin.y;
    const cl_int origin_x = job.input_origin.x;
    const cl_int tile_h   = job.input_h;
    const cl_int tile_w   = job.input_w;
    module.ForwardReflectHwc3ToHwc(dispatch.input_aligned_hwc, frame_h, frame_w, origin_y, origin_x,
                                   tile_h, tile_w, state.tile_output.get(), activation_slots, queue,
                                   /*apply_gamma_decode=*/true);

    const cl_int canvas_w = dispatch.aligned_width;
    const cl_int canvas_h = dispatch.aligned_height;
    const cl_int dst_x    = job.destination_roi.x;
    const cl_int dst_y    = job.destination_roi.y;
    const cl_int owned_w  = job.destination_roi.width;
    const cl_int owned_h  = job.destination_roi.height;
    const cl_int src_x    = job.model_output_roi.x;
    const cl_int src_y    = job.model_output_roi.y;
    opencl::nn::BeginDemosaicNetStage("tile_assembly");
    SetArg(state.assemble.get(), 0, state.tile_output.get(), "tile assemble arg0");
    SetArg(state.assemble.get(), 1, dispatch.output_aligned_hwc, "tile assemble arg1");
    SetArg(state.assemble.get(), 2, tile_w - 2 * Module::kTileBorder, "tile assemble arg2");
    SetArg(state.assemble.get(), 3, tile_h - 2 * Module::kTileBorder, "tile assemble arg3");
    SetArg(state.assemble.get(), 4, canvas_w, "tile assemble arg4");
    SetArg(state.assemble.get(), 5, canvas_h, "tile assemble arg5");
    SetArg(state.assemble.get(), 6, dst_x, "tile assemble arg6");
    SetArg(state.assemble.get(), 7, dst_y, "tile assemble arg7");
    SetArg(state.assemble.get(), 8, owned_w, "tile assemble arg8");
    SetArg(state.assemble.get(), 9, owned_h, "tile assemble arg9");
    SetArg(state.assemble.get(), 10, src_x, "tile assemble arg10");
    SetArg(state.assemble.get(), 11, src_y, "tile assemble arg11");
    Enqueue2D(state.assemble.get(), owned_w, owned_h, queue, "tile assembly enqueue");
    opencl::nn::FinishDemosaicNetStage("tile_assembly", queue);
  }

  return {.tile_count    = jobs.size(),
          .output_width  = dispatch.aligned_width,
          .output_height = dispatch.aligned_height};
}

}  // namespace

auto OpenClDemosaicNetTiledExecutor::EnqueueBayer(const OpenClBayerDemosaicNet& module,
                                                  opencl::nn::ActivationSlots&  activation_slots,
                                                  const OpenClDemosaicNetTiledDispatch& dispatch)
    -> OpenClDemosaicNetTiledResult {
  return EnqueueTiles(*impl_, module, activation_slots, dispatch,
                      detail::MakeBayerStudentTilePolicy());
}

auto OpenClDemosaicNetTiledExecutor::EnqueueXTrans(const OpenClXTransDemosaicNet& module,
                                                   opencl::nn::ActivationSlots&   activation_slots,
                                                   const OpenClDemosaicNetTiledDispatch& dispatch)
    -> OpenClDemosaicNetTiledResult {
  return EnqueueTiles(*impl_, module, activation_slots, dispatch,
                      detail::MakeXTransStudentTilePolicy());
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
