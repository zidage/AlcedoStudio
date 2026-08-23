//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_CUDA

#include "edit/scope/scope_analyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "utils/diagnostics/scope_diag.hpp"

#include "edit/scope/detail/scope_cuda_shared.cuh"

namespace alcedo {
namespace {

constexpr int kScopeSlotCount = 3;

inline auto HasScopeEnabled(const ScopeRequest& request, ScopeType type) -> bool {
  return (request.enabled_mask & static_cast<uint32_t>(type)) != 0U;
}

inline auto ClampPositive(int value, int fallback) -> int { return value > 0 ? value : fallback; }

__global__ void AccumulateHistogramKernel(const float4* input, size_t input_pitch_in_pixels,
                                          int width, int height, int sample_step, int bins,
                                          uint32_t* histogram_counts) {
  const int sample_x = blockIdx.x * blockDim.x + threadIdx.x;
  const int sample_y = blockIdx.y * blockDim.y + threadIdx.y;
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height) {
    return;
  }

  const float4 pixel = input[static_cast<size_t>(y) * input_pitch_in_pixels + x];
  const float  rgb[3] = {fminf(fmaxf(pixel.x, 0.0f), 1.0f), fminf(fmaxf(pixel.y, 0.0f), 1.0f),
                         fminf(fmaxf(pixel.z, 0.0f), 1.0f)};
  for (int channel = 0; channel < 3; ++channel) {
    const int bin = min(static_cast<int>(rgb[channel] * static_cast<float>(bins - 1) + 0.5f),
                        bins - 1);
    atomicAdd(&histogram_counts[channel * bins + bin], 1U);
  }
}

__global__ void AccumulateWaveformKernel(const float4* input, size_t input_pitch_in_pixels, int width,
                                         int height, int sample_step, float4* waveform,
                                         size_t waveform_pitch_in_pixels, int waveform_width,
                                         int waveform_height) {
  const int sample_x = blockIdx.x * blockDim.x + threadIdx.x;
  const int sample_y = blockIdx.y * blockDim.y + threadIdx.y;
  const int x        = sample_x * sample_step;
  const int y        = sample_y * sample_step;
  if (x >= width || y >= height) {
    return;
  }

  const float4 pixel = input[static_cast<size_t>(y) * input_pitch_in_pixels + x];
  const float  rgb[3] = {fminf(fmaxf(pixel.x, 0.0f), 1.0f), fminf(fmaxf(pixel.y, 0.0f), 1.0f),
                         fminf(fmaxf(pixel.z, 0.0f), 1.0f)};
  const int x_bin =
      min(static_cast<int>((static_cast<float>(x) / max(1, width - 1)) * (waveform_width - 1) + 0.5f),
          waveform_width - 1);

  for (int channel = 0; channel < 3; ++channel) {
    const int y_bin = waveform_height - 1 -
                      min(static_cast<int>(rgb[channel] * static_cast<float>(waveform_height - 1) +
                                           0.5f),
                          waveform_height - 1);
    float4* target =
        &waveform[static_cast<size_t>(y_bin) * waveform_pitch_in_pixels + x_bin];
    if (channel == 0) {
      atomicAdd(&target->x, 1.0f);
    } else if (channel == 1) {
      atomicAdd(&target->y, 1.0f);
    } else {
      atomicAdd(&target->z, 1.0f);
    }
    atomicAdd(&target->w, 1.0f);
  }
}

struct ScopeSlot {
  std::shared_ptr<scope::cuda_detail::CudaLinearImageResource>  input_image    = {};
  std::shared_ptr<scope::cuda_detail::CudaDeviceBufferResource> histogram      = {};
  std::shared_ptr<scope::cuda_detail::CudaLinearImageResource>  waveform       = {};
  // Shared with the staged FinalDisplayFrameView's GpuSignalHandle so the
  // readiness event outlives the analyzer slot while the tap still holds the
  // staged frame. Carries this slot's index for deferred SubmitFrame lookup.
  std::shared_ptr<scope::cuda_detail::CudaEventHandleResource>  input_ready    = {};
  cudaEvent_t                                                   analysis_done  = nullptr;
  int                                                           input_width    = 0;
  int                                                           input_height   = 0;
  int                                                           histogram_bins = 0;
  int                                                           waveform_width = 0;
  int                                                           waveform_height = 0;
  uint64_t                                                      generation     = 0;
  uint64_t                                                      image_identity = 0;
  uint64_t                                                      session_epoch = 0;
  uint64_t                                                      display_generation = 0;

  // Idle: free for staging. Staged: StageFrame copied input and recorded
  // input_ready; awaiting SubmitFrame. Dispatched: SubmitFrame dispatched
  // analysis; awaiting analysis_done before reuse.
  enum class Phase { Idle, Staged, Dispatched };
  Phase phase = Phase::Idle;

  void EnsureAnalysisDoneEvent() {
    if (!analysis_done) {
      cudaEventCreateWithFlags(&analysis_done, cudaEventDisableTiming);
      cudaEventRecord(analysis_done, nullptr);
    }
  }

  void EnsureInputReadyEvent() {
    if (!input_ready) {
      input_ready = std::make_shared<scope::cuda_detail::CudaEventHandleResource>();
      cudaEventCreateWithFlags(&input_ready->event, cudaEventDisableTiming);
    }
  }

  void ResetResources() {
    input_image.reset();
    histogram.reset();
    waveform.reset();
    input_ready.reset();
    input_width = 0;
    input_height = 0;
    histogram_bins = 0;
    waveform_width = 0;
    waveform_height = 0;
    generation = 0;
    image_identity = 0;
    session_epoch = 0;
    display_generation = 0;
    phase = Phase::Idle;
  }

  void Release() {
    ResetResources();
    if (analysis_done) {
      cudaEventDestroy(analysis_done);
      analysis_done = nullptr;
    }
  }
};

class CudaScopeAnalyzerImpl final : public IScopeAnalyzer {
 public:
  CudaScopeAnalyzerImpl() {
    int lowest_priority = 0;
    int highest_priority = 0;
    if (cudaDeviceGetStreamPriorityRange(&lowest_priority, &highest_priority) == cudaSuccess) {
      if (cudaStreamCreateWithPriority(&analysis_stream_, cudaStreamNonBlocking, lowest_priority) !=
          cudaSuccess) {
        cudaStreamCreateWithFlags(&analysis_stream_, cudaStreamNonBlocking);
      }
    } else {
      cudaStreamCreateWithFlags(&analysis_stream_, cudaStreamNonBlocking);
    }

    for (auto& slot : slots_) {
      slot.EnsureAnalysisDoneEvent();
    }
  }

  ~CudaScopeAnalyzerImpl() override { ReleaseResources(); }

  // Make an analyzer-owned copy of the pipeline's final display frame on the
  // render thread, while the pipeline source (result_ptr / stream) is still
  // valid. Returns a stable FinalDisplayFrameView backed by the owned slot
  // buffer and a readiness event; the deferred SubmitFrame later analyzes it
  // without touching the pipeline's reused scratch buffers or stream.
  auto StageFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request)
      -> FinalDisplayFrameView override {
    // Pass through frames this analyzer does not own so a composite analyzer
    // can hand them to the matching backend without clobbering them.
    if (!frame || frame.image.backend != GpuBackend::Cuda) {
      return frame;
    }
    if (request.enabled_mask == 0U) {
      return {};
    }

    auto* image_resource =
        static_cast<scope::cuda_detail::CudaLinearImageResource*>(frame.image.resource.get());
    auto* signal_resource = static_cast<scope::cuda_detail::CudaStreamSignalResource*>(
        frame.ready_signal.resource.get());
    if (!image_resource || !signal_resource || !image_resource->device_ptr ||
        !signal_resource->stream) {
      diag::NoteScope("stage_skip missing_cuda_buffer_or_stream");
      return {};
    }

    // Throttle staging to the scope target FPS; the render thread may run
    // faster and the deferred poll only consumes ~target_fps frames anyway.
    const auto now        = std::chrono::steady_clock::now();
    const int  target_fps = std::max(0, request.target_fps);
    if (target_fps > 0 && last_stage_time_.time_since_epoch().count() != 0) {
      const auto min_interval = std::chrono::milliseconds(1000 / target_fps);
      if ((now - last_stage_time_) < min_interval) {
        return {};
      }
    }

    const int frame_width  = ClampPositive(frame.width, image_resource->width);
    const int frame_height = ClampPositive(frame.height, image_resource->height);
    if (frame_width <= 0 || frame_height <= 0) {
      return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ReclaimCompletedSlots();
    ScopeSlot* slot = AcquireIdleSlot();
    if (!slot) {
      diag::NoteScope("stage_skip no_idle_slot");
      return {};
    }

    EnsureSlotStorage(*slot, frame_width, frame_height, request);
    slot->EnsureInputReadyEvent();
    slot->input_ready->slot_index = static_cast<int>(slot - &slots_[0]);

    const size_t input_row_bytes = static_cast<size_t>(frame_width) * sizeof(float4);
    const auto   copy_status     = cudaMemcpy2DAsync(
        slot->input_image->device_ptr, slot->input_image->row_bytes, image_resource->device_ptr,
        image_resource->row_bytes, input_row_bytes, static_cast<size_t>(frame_height),
        cudaMemcpyDeviceToDevice, signal_resource->stream);
    if (copy_status != cudaSuccess) {
      slot->phase = ScopeSlot::Phase::Idle;
      diag::NoteScope(std::string("stage_skip memcpy=") + cudaGetErrorString(copy_status));
      return {};
    }
    cudaEventRecord(slot->input_ready->event, signal_resource->stream);

    slot->image_identity     = frame.image_identity;
    slot->session_epoch   = frame.session_epoch;
    slot->display_generation = frame.display_generation;
    slot->generation         = next_generation_++;
    slot->phase              = ScopeSlot::Phase::Staged;
    last_stage_time_         = now;
    current_request_         = request;

    FinalDisplayFrameView staged = frame;
    staged.image = SharedGpuImageHandle{GpuBackend::Cuda, slot->input_image, frame_width,
                                         frame_height, slot->input_image->row_bytes,
                                         FramePixelFormat::RGBA32F};
    staged.ready_signal = GpuSignalHandle{GpuBackend::Cuda, slot->input_ready};
    staged.width  = frame_width;
    staged.height = frame_height;
    return staged;
  }

  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) override {
    if (!frame || frame.image.backend != GpuBackend::Cuda || request.enabled_mask == 0U ||
        !analysis_stream_) {
      return;
    }
    auto* event_resource = static_cast<scope::cuda_detail::CudaEventHandleResource*>(
        frame.ready_signal.resource.get());
    auto* image_resource =
        static_cast<scope::cuda_detail::CudaLinearImageResource*>(frame.image.resource.get());
    if (!event_resource || event_resource->slot_index < 0 || !image_resource ||
        !image_resource->device_ptr) {
      diag::NoteScope("submit_skip missing_staged_event");
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (event_resource->slot_index >= static_cast<int>(slots_.size())) {
      return;
    }
    ScopeSlot& slot = slots_[event_resource->slot_index];
    // Only analyze a frame still staged for this slot with the matching
    // readiness event. A newer staging that reused the slot produced a fresh
    // event, so a stale in-flight task bails without reading overwritten data.
    if (slot.phase != ScopeSlot::Phase::Staged || slot.input_ready.get() != event_resource) {
      return;
    }

    const int frame_width  = ClampPositive(frame.width, image_resource->width);
    const int frame_height = ClampPositive(frame.height, image_resource->height);
    EnsureSlotStorage(slot, frame_width, frame_height, request);

    cudaStreamWaitEvent(analysis_stream_, slot.input_ready->event, 0);
    if (slot.histogram && HasScopeEnabled(request, ScopeType::Histogram)) {
      cudaMemsetAsync(slot.histogram->device_ptr, 0, slot.histogram->size_bytes, analysis_stream_);
    }
    if (slot.waveform && HasScopeEnabled(request, ScopeType::Waveform)) {
      cudaMemsetAsync(slot.waveform->device_ptr, 0,
                      static_cast<size_t>(slot.waveform_height) * slot.waveform->row_bytes,
                      analysis_stream_);
    }

    const int sample_step = std::max(1, request.analysis_downsample);
    const dim3 block_dim(16, 16);
    const dim3 grid_dim((frame_width + sample_step * block_dim.x - 1) / (sample_step * block_dim.x),
                        (frame_height + sample_step * block_dim.y - 1) /
                            (sample_step * block_dim.y));

    if (slot.histogram && HasScopeEnabled(request, ScopeType::Histogram)) {
      AccumulateHistogramKernel<<<grid_dim, block_dim, 0, analysis_stream_>>>(
          static_cast<const float4*>(slot.input_image->device_ptr),
          slot.input_image->row_bytes / sizeof(float4), frame_width, frame_height, sample_step,
          slot.histogram_bins, static_cast<uint32_t*>(slot.histogram->device_ptr));
    }

    if (slot.waveform && HasScopeEnabled(request, ScopeType::Waveform)) {
      AccumulateWaveformKernel<<<grid_dim, block_dim, 0, analysis_stream_>>>(
          static_cast<const float4*>(slot.input_image->device_ptr),
          slot.input_image->row_bytes / sizeof(float4), frame_width, frame_height, sample_step,
          static_cast<float4*>(slot.waveform->device_ptr), slot.waveform->row_bytes / sizeof(float4),
          slot.waveform_width, slot.waveform_height);
    }

    cudaEventRecord(slot.analysis_done, analysis_stream_);
    slot.phase         = ScopeSlot::Phase::Dispatched;
    current_request_   = request;
  }

  auto GetLatestOutput() -> ScopeOutputSet override {
    std::lock_guard<std::mutex> lock(mutex_);
    ReclaimCompletedSlots();

    ScopeSlot* latest_slot = nullptr;
    for (auto& slot : slots_) {
      if (slot.phase != ScopeSlot::Phase::Dispatched || !slot.analysis_done) {
        continue;
      }
      if (cudaEventQuery(slot.analysis_done) != cudaSuccess) {
        continue;
      }
      if (!latest_slot || slot.generation > latest_slot->generation) {
        latest_slot = &slot;
      }
    }

    if (!latest_slot) {
      return {};
    }
    consumed_generation_ = std::max(consumed_generation_, latest_slot->generation);

    ScopeOutputSet output;
    output.generation      = latest_slot->generation;
    output.histogram_bins = latest_slot->histogram_bins;
    output.waveform_width = latest_slot->waveform_width;
    output.waveform_height = latest_slot->waveform_height;
    output.image_identity     = latest_slot->image_identity;
    output.session_epoch   = latest_slot->session_epoch;
    output.display_generation = latest_slot->display_generation;

    if (latest_slot->histogram) {
      output.histogram_buffer.backend    = GpuBackend::Cuda;
      output.histogram_buffer.resource   = latest_slot->histogram;
      output.histogram_buffer.size_bytes = latest_slot->histogram->size_bytes;
      output.histogram_valid             = true;
    }

    if (latest_slot->waveform) {
      output.waveform_image.backend   = GpuBackend::Cuda;
      output.waveform_image.resource  = latest_slot->waveform;
      output.waveform_image.width     = latest_slot->waveform_width;
      output.waveform_image.height    = latest_slot->waveform_height;
      output.waveform_image.row_bytes = latest_slot->waveform->row_bytes;
      output.waveform_image.format    = FramePixelFormat::RGBA32F;
      output.waveform_valid           = true;
    }

    return output;
  }

  void ResizeResources(const ScopeRequest& request) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (analysis_stream_) {
      cudaStreamSynchronize(analysis_stream_);
    }
    current_request_ = request;
    for (auto& slot : slots_) {
      slot.ResetResources();
    }
  }

  void ReleaseResources() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (analysis_stream_) {
      cudaStreamSynchronize(analysis_stream_);
    }
    for (auto& slot : slots_) {
      slot.Release();
    }
    if (analysis_stream_) {
      cudaStreamDestroy(analysis_stream_);
      analysis_stream_ = nullptr;
    }
  }

 private:
  void ReclaimCompletedSlots() {
    // Caller holds mutex_. Keep the newest completed Dispatched slot until
    // GetLatestOutput has returned it. Reclaiming on complete used to idle the
    // only live histogram buffer before the deferred poll could copy it, which
    // left the editor stuck on "Reading display scope" during continuous presents.
    for (auto& slot : slots_) {
      slot.EnsureAnalysisDoneEvent();
      if (slot.phase != ScopeSlot::Phase::Dispatched || !slot.analysis_done) {
        continue;
      }
      if (cudaEventQuery(slot.analysis_done) != cudaSuccess) {
        continue;
      }
      if (slot.generation < consumed_generation_) {
        slot.phase = ScopeSlot::Phase::Idle;
      }
    }
  }

  auto AcquireIdleSlot() -> ScopeSlot* {
    for (auto& slot : slots_) {
      slot.EnsureAnalysisDoneEvent();
      if (slot.phase == ScopeSlot::Phase::Idle) {
        return &slot;
      }
    }
    return nullptr;
  }

  void EnsureSlotStorage(ScopeSlot& slot, int frame_width, int frame_height,
                         const ScopeRequest& request) {
    const int histogram_bins  = ClampPositive(request.histogram_bins, 256);
    const int waveform_width  = ClampPositive(request.waveform_width, 384);
    const int waveform_height = ClampPositive(request.waveform_height, 192);

    if (!slot.input_image || slot.input_width != frame_width || slot.input_height != frame_height) {
      slot.input_image = std::make_shared<scope::cuda_detail::CudaLinearImageResource>();
      slot.input_image->row_bytes = static_cast<size_t>(frame_width) * sizeof(float4);
      slot.input_image->width     = frame_width;
      slot.input_image->height    = frame_height;
      slot.input_image->format    = FramePixelFormat::RGBA32F;
      slot.input_image->owns_memory = true;
      cudaMalloc(&slot.input_image->device_ptr,
                 static_cast<size_t>(frame_height) * slot.input_image->row_bytes);
      slot.input_width  = frame_width;
      slot.input_height = frame_height;
    }

    if (HasScopeEnabled(request, ScopeType::Histogram)) {
      const size_t histogram_bytes = static_cast<size_t>(histogram_bins) * 3U * sizeof(uint32_t);
      if (!slot.histogram || slot.histogram_bins != histogram_bins ||
          slot.histogram->size_bytes != histogram_bytes) {
        slot.histogram = std::make_shared<scope::cuda_detail::CudaDeviceBufferResource>();
        slot.histogram->size_bytes  = histogram_bytes;
        slot.histogram->owns_memory = true;
        cudaMalloc(&slot.histogram->device_ptr, histogram_bytes);
        slot.histogram_bins = histogram_bins;
      }
    } else {
      slot.histogram.reset();
      slot.histogram_bins = 0;
    }

    if (HasScopeEnabled(request, ScopeType::Waveform)) {
      const size_t waveform_row_bytes = static_cast<size_t>(waveform_width) * sizeof(float4);
      if (!slot.waveform || slot.waveform_width != waveform_width ||
          slot.waveform_height != waveform_height || slot.waveform->row_bytes != waveform_row_bytes) {
        slot.waveform = std::make_shared<scope::cuda_detail::CudaLinearImageResource>();
        slot.waveform->row_bytes   = waveform_row_bytes;
        slot.waveform->width       = waveform_width;
        slot.waveform->height      = waveform_height;
        slot.waveform->format      = FramePixelFormat::RGBA32F;
        slot.waveform->owns_memory = true;
        cudaMalloc(&slot.waveform->device_ptr,
                   static_cast<size_t>(waveform_height) * waveform_row_bytes);
        slot.waveform_width  = waveform_width;
        slot.waveform_height = waveform_height;
      }
    } else {
      slot.waveform.reset();
      slot.waveform_width  = 0;
      slot.waveform_height = 0;
    }
  }

  std::array<ScopeSlot, kScopeSlotCount>        slots_{};
  cudaStream_t                                  analysis_stream_ = nullptr;
  ScopeRequest                                  current_request_{};
  std::chrono::steady_clock::time_point         last_stage_time_{};
  uint64_t                                      next_generation_      = 1;
  uint64_t                                      consumed_generation_  = 0;
  std::mutex                                    mutex_{};
};

}  // namespace

auto CreateCudaScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  return std::make_shared<CudaScopeAnalyzerImpl>();
}

}  // namespace alcedo

#endif
