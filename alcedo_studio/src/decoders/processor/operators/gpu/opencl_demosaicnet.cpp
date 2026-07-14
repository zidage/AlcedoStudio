//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_demosaicnet.hpp"

#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "opencl/nn/common.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/nn/workspace.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"

namespace alcedo::OpenCL {
namespace {

// One active Neural decode: mutable cl_kernel arguments and the grow-only workspace
// are shared on the single in-order queue. Product RAW processing is already serialized;
// this mutex covers harness/test re-entry as well.
std::mutex g_neural_decode_mutex;

std::uint64_t g_success_count          = 0;
std::uint64_t g_fallback_ready_count   = 0;
std::uint64_t g_host_wait_count        = 0;
std::uint64_t g_legacy_fallback_count  = 0;

class KernelHolder {
 public:
  KernelHolder() = default;
  ~KernelHolder() { Reset(); }
  KernelHolder(const KernelHolder&)            = delete;
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
  }

  [[nodiscard]] auto get() const -> cl_kernel { return kernel_; }

 private:
  void Reset() noexcept {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
      kernel_ = nullptr;
    }
  }
  cl_kernel kernel_ = nullptr;
};

template <typename T>
void SetArg(cl_kernel kernel, cl_uint index, const T& value, const char* what) {
  opencl::nn::CheckOpenCl(clSetKernelArg(kernel, index, sizeof(T), &value), what);
}

auto StructuralProgram() -> cl_program {
  RegisterOpenClBackendPrograms();
  return OpenClProgramLibrary::Instance().GetProgram(DemosaicNet::kStructuralProgramName);
}

void EnqueueClamp01(cl_mem buffer, const int count, cl_command_queue queue) {
  if (count <= 0) {
    return;
  }
  KernelHolder kernel;
  kernel.Create(StructuralProgram(), DemosaicNet::kClamp01KernelName);
  SetArg(kernel.get(), 0, buffer, "clamp01 arg0");
  SetArg(kernel.get(), 1, count, "clamp01 arg1");
  const size_t global = static_cast<size_t>(count);
  opencl::nn::CheckOpenCl(
      clEnqueueNDRangeKernel(queue, kernel.get(), 1, nullptr, &global, nullptr, 0, nullptr, nullptr),
      "clamp01 enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
}

void EnqueuePackMonoToHwc3(cl_mem mono, cl_mem hwc3, int width, int height, int period,
                           const int* rgb_fc, cl_command_queue queue) {
  KernelHolder kernel;
  kernel.Create(StructuralProgram(), DemosaicNet::kPackCfaMonoToHwc3KernelName);

  const std::size_t table_bytes = static_cast<std::size_t>(period) * period * sizeof(int);
  cl_int            err         = CL_SUCCESS;
  cl_mem table = clCreateBuffer(OpenClContext::Instance().Context(),
                                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, table_bytes,
                                const_cast<int*>(rgb_fc), &err);
  opencl::nn::CheckOpenCl(err, "pack mono table buffer");

  SetArg(kernel.get(), 0, mono, "pack mono arg0");
  SetArg(kernel.get(), 1, hwc3, "pack mono arg1");
  SetArg(kernel.get(), 2, width, "pack mono arg2");
  SetArg(kernel.get(), 3, height, "pack mono arg3");
  SetArg(kernel.get(), 4, period, "pack mono arg4");
  SetArg(kernel.get(), 5, table, "pack mono arg5");
  const size_t global[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  err = clEnqueueNDRangeKernel(queue, kernel.get(), 2, nullptr, global, nullptr, 0, nullptr,
                               nullptr);
  clReleaseMemObject(table);
  opencl::nn::CheckOpenCl(err, "pack mono enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
}

void EnqueueRgb3ToRgba4(cl_mem rgb, cl_mem rgba, int width, int height, cl_command_queue queue) {
  KernelHolder kernel;
  kernel.Create(StructuralProgram(), DemosaicNet::kRgb3ToRgba4KernelName);
  SetArg(kernel.get(), 0, rgb, "rgb2rgba arg0");
  SetArg(kernel.get(), 1, rgba, "rgb2rgba arg1");
  SetArg(kernel.get(), 2, width, "rgb2rgba arg2");
  SetArg(kernel.get(), 3, height, "rgb2rgba arg3");
  const size_t global[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  opencl::nn::CheckOpenCl(
      clEnqueueNDRangeKernel(queue, kernel.get(), 2, nullptr, global, nullptr, 0, nullptr, nullptr),
      "rgb2rgba enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
}

auto VariantName(const RawCfaKind kind) -> const char* {
  return kind == RawCfaKind::XTrans6x6 ? "XTrans" : "Bayer";
}

void FillRgbFcTable(const RawCfaPattern& training, std::vector<int>& table, int& period) {
  period = CfaPeriod(training.kind);
  table.resize(static_cast<std::size_t>(period) * period);
  for (int y = 0; y < period; ++y) {
    for (int x = 0; x < period; ++x) {
      table[static_cast<std::size_t>(y * period + x)] = RgbColorAt(training, y, x);
    }
  }
}

}  // namespace

void Clamp01(opencl::OpenClImage& image) {
  if (image.Empty()) {
    return;
  }
  auto& context = OpenClContext::Instance();
  if (!context.IsInitialized()) {
    context.Initialize();
  }
  const int channels = CV_MAT_CN(image.Type());
  const int count    = image.Width() * image.Height() * channels;
  EnqueueClamp01(image.Buffer(), count, context.Queue());
  // Boundary helper for product path: wait so subsequent host decisions see clamped data
  // only when callers require it. Product Neural path enqueues further work on the same
  // in-order queue without requiring an intermediate wait; keep this non-blocking here.
}

void ResetOpenClNeuralPathCountersForTest() {
  g_success_count         = 0;
  g_fallback_ready_count  = 0;
  g_host_wait_count       = 0;
  g_legacy_fallback_count = 0;
}

auto OpenClNeuralSuccessCountForTest() noexcept -> std::uint64_t { return g_success_count; }

auto OpenClNeuralFallbackReadyCountForTest() noexcept -> std::uint64_t {
  return g_fallback_ready_count;
}

auto OpenClNeuralHostWaitCountForTest() noexcept -> std::uint64_t { return g_host_wait_count; }

void NoteOpenClNeuralLegacyFallbackForTest() { ++g_legacy_fallback_count; }

auto OpenClNeuralLegacyFallbackCountForTest() noexcept -> std::uint64_t {
  return g_legacy_fallback_count;
}

auto DemosaicWithNeuralEngine(const opencl::OpenClImage& linear_cfa,
                              const RawCfaPattern& camera_pattern, opencl::OpenClImage& rgb_rgba,
                              const OpenClNeuralDemosaicOptions& options)
    -> OpenClNeuralDemosaicResult {
  OpenClNeuralDemosaicResult result;
  result.variant = VariantName(camera_pattern.kind);

  std::lock_guard<std::mutex> lock(g_neural_decode_mutex);

  try {
    if (linear_cfa.Empty() || linear_cfa.Type() != CV_32FC1) {
      result.failure_stage = "prepare";
      result.error =
          "OpenCL Neural Engine requires a non-empty CV_32FC1 linear CFA (stage=prepare, variant=" +
          result.variant + ")";
      ++g_fallback_ready_count;
      return result;
    }

    auto& context = OpenClContext::Instance();
    if (!context.IsInitialized()) {
      context.Initialize();
    }
    const cl_command_queue queue = context.Queue();

    const int min_spatial = camera_pattern.kind == RawCfaKind::XTrans6x6
                                ? DemosaicNetXTransSpec::kMinSpatial
                                : DemosaicNetBayerSpec::kMinSpatial;
    std::string geo_error;
    const auto  geo = ComputeNeuralAlignedGeometry(camera_pattern, linear_cfa.Width(),
                                                   linear_cfa.Height(), min_spatial, &geo_error);
    if (!geo.has_value()) {
      result.failure_stage = "prepare";
      result.error =
          "OpenCL Neural Engine prepare failed (stage=prepare, variant=" + result.variant + "): " +
          geo_error;
      ++g_fallback_ready_count;
      return result;
    }

    result.phase_shift_x  = geo->shift_sx;
    result.phase_shift_y  = geo->shift_sy;
    result.aligned_width  = geo->aligned_width;
    result.aligned_height = geo->aligned_height;

    // Private monochrome ROI so the caller's linear CFA stays intact for Legacy fallback.
    opencl::OpenClImage aligned_mono;
    linear_cfa.CropTo(aligned_mono,
                      cv::Rect(geo->shift_sx, geo->shift_sy, geo->aligned_width, geo->aligned_height));

    const RawCfaPattern training = DemosaicNetTrainingPattern(camera_pattern.kind);
    std::vector<int>    rgb_fc;
    int                 period = 0;
    FillRgbFcTable(training, rgb_fc, period);

    opencl::nn::DeviceBuffer mosaic_hwc = opencl::nn::DeviceBuffer::Floats(
        static_cast<std::size_t>(geo->aligned_width) * geo->aligned_height * 3);
    EnqueuePackMonoToHwc3(aligned_mono.Buffer(), mosaic_hwc.get(), geo->aligned_width,
                          geo->aligned_height, period, rgb_fc.data(), queue);

    if (options.injected_failure == OpenClNeuralInjectedFailure::ModelLoad) {
      result.failure_stage = "load";
      result.error =
          "OpenCL Neural Engine injected model-load failure (stage=load, variant=" + result.variant +
          ")";
      ++g_fallback_ready_count;
      return result;
    }

    OpenClDemosaicNetModelCache& cache =
        options.model_cache == nullptr ? OpenClDemosaicNetModelCache::Instance()
                                       : *options.model_cache;
    OpenClDemosaicNetLoadOptions load_options = options.load_options;
    load_options.queue                        = queue;
    const OpenClDemosaicNetVariant variant    = camera_pattern.kind == RawCfaKind::XTrans6x6
                                                    ? OpenClDemosaicNetVariant::XTrans
                                                    : OpenClDemosaicNetVariant::Bayer;
    if (!cache.EnsureLoaded(variant, load_options)) {
      result.failure_stage = "load";
      result.error =
          "OpenCL Neural Engine model load failed (stage=load, variant=" + result.variant + "): " +
          cache.LastError();
      ++g_fallback_ready_count;
      return result;
    }

    if (options.injected_failure == OpenClNeuralInjectedFailure::Enqueue) {
      result.failure_stage = "enqueue";
      result.error =
          "OpenCL Neural Engine injected enqueue failure (stage=enqueue, variant=" + result.variant +
          ")";
      ++g_fallback_ready_count;
      return result;
    }

    opencl::nn::DeviceBuffer rgb_hwc = opencl::nn::DeviceBuffer::Floats(
        static_cast<std::size_t>(geo->aligned_width) * geo->aligned_height * 3);

    opencl::nn::WorkspacePool          workspace;
    OpenClDemosaicNetTiledExecutor     executor;
    OpenClDemosaicNetTiledDispatch     dispatch;
    dispatch.input_aligned_hwc  = mosaic_hwc.get();
    dispatch.output_aligned_hwc = rgb_hwc.get();
    dispatch.aligned_width      = geo->aligned_width;
    dispatch.aligned_height     = geo->aligned_height;
    dispatch.queue              = queue;

    OpenClDemosaicNetTiledResult tiled;
    if (variant == OpenClDemosaicNetVariant::Bayer) {
      tiled = executor.EnqueueBayer(cache.Bayer(), workspace, dispatch);
    } else {
      tiled = executor.EnqueueXTrans(cache.XTrans(), workspace, dispatch);
    }
    result.tile_count = tiled.tile_count;

    // Pack RGBA on the same in-order queue, then one Neural-stage wait.
    opencl::OpenClImage rgba;
    rgba.Create(geo->aligned_width, geo->aligned_height, CV_32FC4);
    EnqueueRgb3ToRgba4(rgb_hwc.get(), rgba.Buffer(), geo->aligned_width, geo->aligned_height,
                       queue);
    opencl::nn::WaitQueue(queue);
    ++g_host_wait_count;

    rgb_rgba         = std::move(rgba);
    result.succeeded = true;
    result.error.clear();
    ++g_success_count;
    return result;
  } catch (const std::exception& e) {
    if (result.failure_stage.empty()) {
      result.failure_stage = "enqueue";
    }
    result.succeeded = false;
    result.error =
        "OpenCL Neural Engine failed (stage=" + result.failure_stage + ", variant=" + result.variant +
        "): " + e.what();
    ++g_fallback_ready_count;
    return result;
  }
}

}  // namespace alcedo::OpenCL

#endif  // HAVE_OPENCL
