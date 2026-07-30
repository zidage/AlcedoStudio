//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "edit/scope/scope_analyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "edit/scope/detail/scope_metal_shared.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

constexpr int      kScopeSlotCount      = 3;
constexpr uint32_t kRowAlignmentBytes   = 256;

struct ScopeAnalysisParams {
  uint32_t input_width;
  uint32_t input_height;
  uint32_t sample_step;
  uint32_t histogram_bins;
  uint32_t waveform_width;
  uint32_t waveform_height;
  uint32_t waveform_stride;
};

enum class Kernel : uint32_t {
  Histogram,
  Waveform,
};

inline auto HasScopeEnabled(const ScopeRequest& request, ScopeType type) -> bool {
  return (request.enabled_mask & static_cast<uint32_t>(type)) != 0U;
}

inline auto ClampPositive(int value, int fallback) -> int { return value > 0 ? value : fallback; }

auto AlignRowBytes(size_t row_bytes) -> size_t {
  return ((row_bytes + kRowAlignmentBytes - 1) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("Metal scope analyzer: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("Metal scope analyzer: failed to allocate shared buffer.");
  }
  return buffer;
}

auto MakeCommandBuffer() -> NS::SharedPtr<MTL::CommandBuffer> {
  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal scope analyzer: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal scope analyzer: failed to create command buffer.");
  }
  return command_buffer;
}

auto KernelNameFor(Kernel kernel) -> const char* {
  switch (kernel) {
    case Kernel::Histogram:
      return "scope_accumulate_histogram";
    case Kernel::Waveform:
      return "scope_accumulate_waveform";
  }

  throw std::runtime_error("Metal scope analyzer: unknown kernel.");
}

auto GetPipelineState(Kernel kernel) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_SCOPE_ANALYZER_METALLIB_PATH
  throw std::runtime_error("Metal scope analyzer metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_SCOPE_ANALYZER_METALLIB_PATH, KernelNameFor(kernel), "Metal scope analyzer");
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }

  const auto thread_width  = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_group{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{width, height, 1};
  encoder->dispatchThreads(threads_per_grid, threads_per_group);
}

auto IsSlotAvailable(const NS::SharedPtr<MTL::CommandBuffer>& command_buffer) -> bool {
  if (!command_buffer) {
    return true;
  }

  const auto status = command_buffer->status();
  return status == MTL::CommandBufferStatusCompleted || status == MTL::CommandBufferStatusError;
}

void WaitForCompletion(const NS::SharedPtr<MTL::CommandBuffer>& command_buffer) {
  if (!command_buffer) {
    return;
  }

  const auto status = command_buffer->status();
  if (status != MTL::CommandBufferStatusCompleted && status != MTL::CommandBufferStatusError) {
    command_buffer->waitUntilCompleted();
  }
}

struct ScopeSlot {
  std::shared_ptr<scope::metal_detail::MetalTextureImageResource> input_image     = {};
  std::shared_ptr<scope::metal_detail::MetalBufferResource>       histogram       = {};
  std::shared_ptr<scope::metal_detail::MetalLinearImageResource>  waveform        = {};
  NS::SharedPtr<MTL::CommandBuffer>                               analysis_buffer = nullptr;
  int                                                             histogram_bins  = 0;
  int                                                             waveform_width  = 0;
  int                                                             waveform_height = 0;
  uint64_t                                                        generation      = 0;
  uint64_t                                                        image_identity  = 0;
  uint64_t                                                        image_generation = 0;
  uint64_t                                                        display_generation = 0;

  void ResetResources() {
    input_image.reset();
    histogram.reset();
    waveform.reset();
    analysis_buffer = nullptr;
    histogram_bins  = 0;
    waveform_width  = 0;
    waveform_height = 0;
    generation      = 0;
    image_identity  = 0;
    image_generation = 0;
    display_generation = 0;
  }
};

class MetalScopeAnalyzerImpl final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) override {
    const bool histogram_enabled = HasScopeEnabled(request, ScopeType::Histogram);
    const bool waveform_enabled  = HasScopeEnabled(request, ScopeType::Waveform);
    if (!frame || frame.image.backend != GpuBackend::Metal || request.enabled_mask == 0U ||
        (!histogram_enabled && !waveform_enabled)) {
      return;
    }

    const auto now        = std::chrono::steady_clock::now();
    const int  target_fps = std::max(0, request.target_fps);
    if (target_fps > 0 && last_submit_time_.time_since_epoch().count() != 0) {
      const auto min_interval = std::chrono::milliseconds(1000 / target_fps);
      if ((now - last_submit_time_) < min_interval) {
        return;
      }
    }

    auto input_image =
        std::shared_ptr<scope::metal_detail::MetalTextureImageResource>(
            frame.image.resource,
            static_cast<scope::metal_detail::MetalTextureImageResource*>(frame.image.resource.get()));
    if (!input_image || !input_image->texture || frame.format != FramePixelFormat::RGBA32F) {
      return;
    }

    const int frame_width  = ClampPositive(frame.width, input_image->width);
    const int frame_height = ClampPositive(frame.height, input_image->height);
    if (frame_width <= 0 || frame_height <= 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScopeSlot* slot = AcquireAvailableSlot();
    if (!slot) {
      return;
    }

    slot->input_image = std::move(input_image);
    EnsureSlotStorage(*slot, request);
    slot->image_identity     = frame.image_identity;
    slot->image_generation   = frame.image_generation;
    slot->display_generation = frame.display_generation;

    auto command_buffer = MakeCommandBuffer();
    const ScopeAnalysisParams params{
        static_cast<uint32_t>(frame_width),
        static_cast<uint32_t>(frame_height),
        static_cast<uint32_t>(std::max(1, request.analysis_downsample)),
        static_cast<uint32_t>(slot->histogram_bins),
        static_cast<uint32_t>(slot->waveform_width),
        static_cast<uint32_t>(slot->waveform_height),
        static_cast<uint32_t>((slot->waveform ? slot->waveform->row_bytes : 0U) /
                              (sizeof(uint32_t) * 4U)),
    };

    if (slot->histogram) {
      std::memset(slot->histogram->buffer->contents(), 0, slot->histogram->size_bytes);
    }
    if (slot->waveform) {
      std::memset(slot->waveform->buffer->contents(), 0,
                  static_cast<size_t>(slot->waveform_height) * slot->waveform->row_bytes);
    }

    const uint32_t sample_grid_width =
        (params.input_width + params.sample_step - 1U) / params.sample_step;
    const uint32_t sample_grid_height =
        (params.input_height + params.sample_step - 1U) / params.sample_step;

    if (slot->histogram && histogram_enabled) {
      auto pipeline = GetPipelineState(Kernel::Histogram);
      auto encoder  = NS::RetainPtr(command_buffer->computeCommandEncoder());
      encoder->setComputePipelineState(pipeline.get());
      encoder->setTexture(slot->input_image->texture.get(), 0);
      encoder->setBuffer(slot->histogram->buffer.get(), 0, 0);
      encoder->setBytes(&params, sizeof(params), 1);
      DispatchThreads(encoder.get(), pipeline.get(), sample_grid_width, sample_grid_height);
      encoder->endEncoding();
    }

    if (slot->waveform && waveform_enabled) {
      auto pipeline = GetPipelineState(Kernel::Waveform);
      auto encoder  = NS::RetainPtr(command_buffer->computeCommandEncoder());
      encoder->setComputePipelineState(pipeline.get());
      encoder->setTexture(slot->input_image->texture.get(), 0);
      encoder->setBuffer(slot->waveform->buffer.get(), 0, 0);
      encoder->setBytes(&params, sizeof(params), 1);
      DispatchThreads(encoder.get(), pipeline.get(), sample_grid_width, sample_grid_height);
      encoder->endEncoding();
    }

    slot->generation      = next_generation_++;
    slot->analysis_buffer = std::move(command_buffer);
    slot->analysis_buffer->commit();
    last_submit_time_ = now;
  }

  auto GetLatestOutput() -> ScopeOutputSet override {
    std::lock_guard<std::mutex> lock(mutex_);

    ScopeSlot* latest_slot = nullptr;
    for (auto& slot : slots_) {
      if (slot.generation == 0 || !slot.analysis_buffer) {
        continue;
      }
      if (slot.analysis_buffer->status() != MTL::CommandBufferStatusCompleted) {
        continue;
      }
      if (!latest_slot || slot.generation > latest_slot->generation) {
        latest_slot = &slot;
      }
    }

    if (!latest_slot) {
      return {};
    }

    ScopeOutputSet output;
    output.generation      = latest_slot->generation;
    output.histogram_bins  = latest_slot->histogram_bins;
    output.waveform_width  = latest_slot->waveform_width;
    output.waveform_height = latest_slot->waveform_height;
    output.image_identity     = latest_slot->image_identity;
    output.image_generation   = latest_slot->image_generation;
    output.display_generation = latest_slot->display_generation;

    if (latest_slot->histogram) {
      output.histogram_buffer.backend    = GpuBackend::Metal;
      output.histogram_buffer.resource   =
          std::shared_ptr<void>(latest_slot->histogram, latest_slot->histogram.get());
      output.histogram_buffer.size_bytes = latest_slot->histogram->size_bytes;
      output.histogram_valid             = true;
    }

    if (latest_slot->waveform) {
      output.waveform_image.backend   = GpuBackend::Metal;
      output.waveform_image.resource  =
          std::shared_ptr<void>(latest_slot->waveform, latest_slot->waveform.get());
      output.waveform_image.width     = latest_slot->waveform_width;
      output.waveform_image.height    = latest_slot->waveform_height;
      output.waveform_image.row_bytes = latest_slot->waveform->row_bytes;
      output.waveform_image.format    = FramePixelFormat::RGBA32F;
      output.waveform_valid           = true;
    }

    return output;
  }

  void ResizeResources(const ScopeRequest&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
      WaitForCompletion(slot.analysis_buffer);
      slot.ResetResources();
    }
  }

  void ReleaseResources() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
      WaitForCompletion(slot.analysis_buffer);
      slot.ResetResources();
    }
  }

 private:
  auto AcquireAvailableSlot() -> ScopeSlot* {
    for (auto& slot : slots_) {
      if (IsSlotAvailable(slot.analysis_buffer)) {
        return &slot;
      }
    }
    return nullptr;
  }

  void EnsureSlotStorage(ScopeSlot& slot, const ScopeRequest& request) {
    const int histogram_bins  = ClampPositive(request.histogram_bins, 256);
    const int waveform_width  = ClampPositive(request.waveform_width, 384);
    const int waveform_height = ClampPositive(request.waveform_height, 192);

    if (HasScopeEnabled(request, ScopeType::Histogram)) {
      const size_t histogram_bytes = static_cast<size_t>(histogram_bins) * 3U * sizeof(uint32_t);
      if (!slot.histogram || slot.histogram_bins != histogram_bins ||
          slot.histogram->size_bytes != histogram_bytes) {
        slot.histogram             = std::make_shared<scope::metal_detail::MetalBufferResource>();
        slot.histogram->buffer     = MakeSharedBuffer(histogram_bytes);
        slot.histogram->size_bytes = histogram_bytes;
        slot.histogram_bins        = histogram_bins;
      }
    } else {
      slot.histogram.reset();
      slot.histogram_bins = 0;
    }

    if (HasScopeEnabled(request, ScopeType::Waveform)) {
      const size_t waveform_row_bytes =
          AlignRowBytes(static_cast<size_t>(waveform_width) * sizeof(uint32_t) * 4U);
      if (!slot.waveform || slot.waveform_width != waveform_width ||
          slot.waveform_height != waveform_height || slot.waveform->row_bytes != waveform_row_bytes) {
        slot.waveform            = std::make_shared<scope::metal_detail::MetalLinearImageResource>();
        slot.waveform->buffer    =
            MakeSharedBuffer(static_cast<size_t>(waveform_height) * waveform_row_bytes);
        slot.waveform->row_bytes = waveform_row_bytes;
        slot.waveform->width     = waveform_width;
        slot.waveform->height    = waveform_height;
        slot.waveform->format    = FramePixelFormat::RGBA32F;
        slot.waveform->storage   = scope::metal_detail::MetalLinearImageStorage::UInt32RGBA;
        slot.waveform_width      = waveform_width;
        slot.waveform_height     = waveform_height;
      }
    } else {
      slot.waveform.reset();
      slot.waveform_width  = 0;
      slot.waveform_height = 0;
    }
  }

  std::array<ScopeSlot, kScopeSlotCount> slots_{};
  std::chrono::steady_clock::time_point  last_submit_time_{};
  uint64_t                               next_generation_ = 1;
  std::mutex                             mutex_{};
};

}  // namespace

auto CreateMetalScopeAnalyzer() -> std::shared_ptr<IScopeAnalyzer> {
  return std::make_shared<MetalScopeAnalyzerImpl>();
}

}  // namespace alcedo

#endif
