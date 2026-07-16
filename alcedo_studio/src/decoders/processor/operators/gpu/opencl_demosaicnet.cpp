//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "decoders/processor/operators/gpu/opencl_demosaicnet.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
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

namespace alcedo::OpenCL {
namespace {

// One active Neural decode: mutable cl_kernel arguments and resident scratch are
// shared on the single in-order queue. Product RAW processing is already serialized;
// this mutex covers harness/test re-entry as well.
std::mutex g_neural_decode_mutex;

std::uint64_t g_success_count         = 0;
std::uint64_t g_fallback_ready_count  = 0;
std::uint64_t g_host_wait_count       = 0;
std::uint64_t g_legacy_fallback_count = 0;

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
    NoteOpenClCreateKernel();
  }

  [[nodiscard]] auto get() const -> cl_kernel { return kernel_; }
  [[nodiscard]] auto empty() const -> bool { return kernel_ == nullptr; }

  void Reset() noexcept {
    if (kernel_ != nullptr) {
      clReleaseKernel(kernel_);
      NoteOpenClReleaseKernel();
      kernel_ = nullptr;
    }
  }

 private:
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

// Context-keyed resident execution state for the product Neural path.
// Retained across hot decodes under g_neural_decode_mutex:
//   structural kernels, CFA lookup, full-frame HWC staging, tiled executor
//   (tile buffers + pack/assemble kernels), and two activation slots.
struct ResidentExecutionState {
  cl_context context = nullptr;

  KernelHolder pack_cfa;
  KernelHolder clamp01;
  KernelHolder rgb_to_rgba;

  opencl::nn::DeviceBuffer cfa_table;
  int                      cfa_period = -1;
  std::vector<int>         cfa_host;

  opencl::nn::DeviceBuffer mosaic_hwc;
  opencl::nn::DeviceBuffer rgb_hwc;

  OpenClDemosaicNetTiledExecutor executor;
  opencl::nn::ActivationSlots    activation_slots;

  void Reset() {
    pack_cfa.Reset();
    clamp01.Reset();
    rgb_to_rgba.Reset();
    cfa_table.Reset();
    cfa_period = -1;
    cfa_host.clear();
    mosaic_hwc.Reset();
    rgb_hwc.Reset();
    executor         = OpenClDemosaicNetTiledExecutor{};
    activation_slots = opencl::nn::ActivationSlots{};
    context          = nullptr;
  }

  void EnsureForContext(OpenClContext& ctx) {
    const cl_context current = ctx.Context();
    if (context != nullptr && context != current) {
      Reset();
    }
    context = current;
    if (pack_cfa.empty()) {
      const cl_program program = StructuralProgram();
      pack_cfa.Create(program, DemosaicNet::kPackCfaMonoToHwc3KernelName);
      clamp01.Create(program, DemosaicNet::kClamp01KernelName);
      rgb_to_rgba.Create(program, DemosaicNet::kRgb3ToRgba4KernelName);
    }
  }

  void EnsureCfaTable(const int period, const std::vector<int>& rgb_fc, cl_command_queue queue) {
    if (period <= 0) {
      throw std::runtime_error("OpenCL Neural Engine: invalid CFA period");
    }
    const std::size_t need = static_cast<std::size_t>(period) * period;
    if (rgb_fc.size() != need) {
      throw std::runtime_error("OpenCL Neural Engine: CFA table size mismatch");
    }
    const bool same = cfa_period == period && cfa_host.size() == need &&
                      std::equal(cfa_host.begin(), cfa_host.end(), rgb_fc.begin());
    if (same && !cfa_table.empty()) {
      return;
    }
    cfa_table.EnsureBytes(need * sizeof(int));
    cfa_table.UploadBytes(rgb_fc.data(), need * sizeof(int), queue, /*blocking=*/true);
    cfa_period = period;
    cfa_host   = rgb_fc;
  }

  void EnsureStagingFloats(const std::size_t hwc3_floats) {
    mosaic_hwc.EnsureBytes(hwc3_floats * sizeof(float));
    rgb_hwc.EnsureBytes(hwc3_floats * sizeof(float));
  }
};

auto ResidentState() -> ResidentExecutionState& {
  static ResidentExecutionState state;
  return state;
}

void EnqueueClamp01(ResidentExecutionState& state, cl_mem buffer, const int count,
                    cl_command_queue queue) {
  if (count <= 0) {
    return;
  }
  SetArg(state.clamp01.get(), 0, buffer, "clamp01 arg0");
  SetArg(state.clamp01.get(), 1, count, "clamp01 arg1");
  const size_t                 global = static_cast<size_t>(count);
  opencl::nn::ScopedStageEvent stage_event;
  opencl::nn::CheckOpenCl(clEnqueueNDRangeKernel(queue, state.clamp01.get(), 1, nullptr, &global,
                                                 nullptr, 0, nullptr, stage_event.out()),
                          "clamp01 enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
  NoteOpenClEnqueueNdRange();
}

void EnqueuePackMonoRoiToHwc3(ResidentExecutionState& state, cl_mem mono, cl_mem hwc3,
                              int src_width, int src_height, int crop_x, int crop_y, int width,
                              int height, int period, cl_command_queue queue) {
  SetArg(state.pack_cfa.get(), 0, mono, "pack mono arg0");
  SetArg(state.pack_cfa.get(), 1, hwc3, "pack mono arg1");
  SetArg(state.pack_cfa.get(), 2, src_width, "pack mono arg2");
  SetArg(state.pack_cfa.get(), 3, src_height, "pack mono arg3");
  SetArg(state.pack_cfa.get(), 4, crop_x, "pack mono arg4");
  SetArg(state.pack_cfa.get(), 5, crop_y, "pack mono arg5");
  SetArg(state.pack_cfa.get(), 6, width, "pack mono arg6");
  SetArg(state.pack_cfa.get(), 7, height, "pack mono arg7");
  SetArg(state.pack_cfa.get(), 8, period, "pack mono arg8");
  cl_mem table = state.cfa_table.get();
  SetArg(state.pack_cfa.get(), 9, table, "pack mono arg9");
  const size_t                 global[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  opencl::nn::ScopedStageEvent stage_event;
  opencl::nn::CheckOpenCl(clEnqueueNDRangeKernel(queue, state.pack_cfa.get(), 2, nullptr, global,
                                                 nullptr, 0, nullptr, stage_event.out()),
                          "pack mono enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
  NoteOpenClEnqueueNdRange();
}

void EnqueueRgb3ToRgba4(ResidentExecutionState& state, cl_mem rgb, cl_mem rgba, int width,
                        int height, cl_command_queue queue) {
  SetArg(state.rgb_to_rgba.get(), 0, rgb, "rgb2rgba arg0");
  SetArg(state.rgb_to_rgba.get(), 1, rgba, "rgb2rgba arg1");
  SetArg(state.rgb_to_rgba.get(), 2, width, "rgb2rgba arg2");
  SetArg(state.rgb_to_rgba.get(), 3, height, "rgb2rgba arg3");
  const size_t                 global[2] = {static_cast<size_t>(width), static_cast<size_t>(height)};
  opencl::nn::ScopedStageEvent stage_event;
  opencl::nn::CheckOpenCl(clEnqueueNDRangeKernel(queue, state.rgb_to_rgba.get(), 2, nullptr, global,
                                                 nullptr, 0, nullptr, stage_event.out()),
                          "rgb2rgba enqueue");
  ++opencl::nn::GetDispatchInstrumentation().enqueue_count;
  NoteOpenClEnqueueNdRange();
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
  std::lock_guard<std::mutex> lock(g_neural_decode_mutex);
  auto&                       state = ResidentState();
  state.EnsureForContext(context);
  const int channels = CV_MAT_CN(image.Type());
  const int count    = image.Width() * image.Height() * channels;
  EnqueueClamp01(state, image.Buffer(), count, context.Queue());
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
    if (auto* const profiler = opencl::nn::ActiveDemosaicNetStageProfiler(); profiler != nullptr) {
      profiler->Drain(queue);
    }

    auto& state = ResidentState();
    state.EnsureForContext(context);

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

    // Fused phase crop + HWC pack: no materialised aligned_mono temporary.
    opencl::nn::BeginDemosaicNetStage("phase_crop_and_hwc_pack");
    const RawCfaPattern training = DemosaicNetTrainingPattern(camera_pattern.kind);
    std::vector<int>    rgb_fc;
    int                 period = 0;
    FillRgbFcTable(training, rgb_fc, period);
    state.EnsureCfaTable(period, rgb_fc, queue);

    const std::size_t hwc3_floats =
        static_cast<std::size_t>(geo->aligned_width) * geo->aligned_height * 3;
    state.EnsureStagingFloats(hwc3_floats);
    EnqueuePackMonoRoiToHwc3(state, linear_cfa.Buffer(), state.mosaic_hwc.get(), linear_cfa.Width(),
                             linear_cfa.Height(), geo->shift_sx, geo->shift_sy, geo->aligned_width,
                             geo->aligned_height, period, queue);
    opencl::nn::FinishDemosaicNetStage("phase_crop_and_hwc_pack", queue);

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

    OpenClDemosaicNetTiledDispatch dispatch;
    dispatch.input_aligned_hwc  = state.mosaic_hwc.get();
    dispatch.output_aligned_hwc = state.rgb_hwc.get();
    dispatch.aligned_width      = geo->aligned_width;
    dispatch.aligned_height     = geo->aligned_height;
    dispatch.queue              = queue;

    OpenClDemosaicNetTiledResult tiled;
    if (variant == OpenClDemosaicNetVariant::Bayer) {
      tiled = state.executor.EnqueueBayer(cache.Bayer(), state.activation_slots, dispatch);
    } else {
      tiled = state.executor.EnqueueXTrans(cache.XTrans(), state.activation_slots, dispatch);
    }
    result.tile_count = tiled.tile_count;

    // Caller-owned final RGBA: Create reuses capacity after warm-up for the same size.
    opencl::nn::BeginDemosaicNetStage("rgb_to_rgba");
    const bool final_was_ready =
        !rgb_rgba.Empty() && rgb_rgba.Width() == geo->aligned_width &&
        rgb_rgba.Height() == geo->aligned_height && rgb_rgba.Type() == CV_32FC4;
    rgb_rgba.Create(geo->aligned_width, geo->aligned_height, CV_32FC4);
    if (!final_was_ready) {
      // OpenClImage::Create already counted create_buffer; reclassify as final output.
      NoteOpenClCreateBufferFinalOutput();
    }
    EnqueueRgb3ToRgba4(state, state.rgb_hwc.get(), rgb_rgba.Buffer(), geo->aligned_width,
                       geo->aligned_height, queue);
    opencl::nn::FinishDemosaicNetStage("rgb_to_rgba", queue);
    opencl::nn::WaitQueue(queue);
    ++g_host_wait_count;

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
