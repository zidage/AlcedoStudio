//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <alcedo/metal/Metal.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>

#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo::metal_detail {

class MetalKernelHandle {
 public:
  MetalKernelHandle(const char* metallib_path, const char* kernel_name, const char* debug_label)
      : metallib_path_(metallib_path), kernel_name_(kernel_name), debug_label_(debug_label) {}

  MetalKernelHandle(const MetalKernelHandle&)                    = delete;
  auto operator=(const MetalKernelHandle&) -> MetalKernelHandle& = delete;

  void Ensure(const char* owner_label) {
    if (pipeline_) {
      return;
    }
    if (metallib_path_ == nullptr || metallib_path_[0] == '\0') {
      throw std::runtime_error(std::string(owner_label) +
                               ": Metal metallib path is not configured.");
    }
    pipeline_ = metal::ComputePipelineCache::Instance().GetPipelineState(
        metallib_path_, kernel_name_, debug_label_ != nullptr ? debug_label_ : owner_label);
    if (!pipeline_) {
      throw std::runtime_error(std::string(owner_label) + ": failed to create Metal pipeline '" +
                               kernel_name_ + "'.");
    }
  }

  void               Release() { pipeline_ = nullptr; }

  [[nodiscard]] auto Get() const -> MTL::ComputePipelineState* { return pipeline_.get(); }

 private:
  const char*                              metallib_path_ = nullptr;
  const char*                              kernel_name_   = nullptr;
  const char*                              debug_label_   = nullptr;
  NS::SharedPtr<MTL::ComputePipelineState> pipeline_      = nullptr;
};

template <typename Derived>
class MetalKernelStage {
 protected:
  static auto MakeCommandBuffer() -> NS::SharedPtr<MTL::CommandBuffer> {
    auto* queue = MetalContext::Instance().Queue();
    if (queue == nullptr) {
      throw std::runtime_error(std::string(Derived::kStageLabel) + ": Metal queue is unavailable.");
    }
    auto command_buffer = NS::RetainPtr(queue->commandBuffer());
    if (!command_buffer) {
      throw std::runtime_error(std::string(Derived::kStageLabel) +
                               ": failed to create command buffer.");
    }
    return command_buffer;
  }

  static auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
    return MakeBuffer(length, MTL::ResourceStorageModeShared, "shared");
  }

  static auto MakeDeviceBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
    return MakeBuffer(length, MTL::ResourceStorageModePrivate, "device");
  }

  static auto EnsurePipeline(MetalKernelHandle& handle) -> MTL::ComputePipelineState* {
    handle.Ensure(Derived::kStageLabel);
    auto* pipeline = handle.Get();
    if (pipeline == nullptr) {
      throw std::runtime_error(std::string(Derived::kStageLabel) +
                               ": Metal pipeline handle is null.");
    }
    return pipeline;
  }

  static void DispatchThreads(MTL::ComputeCommandEncoder* encoder,
                              MTL::ComputePipelineState* pipeline, uint32_t width,
                              uint32_t height) {
    const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
    const auto thread_height =
        std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
    const MTL::Size threads_per_group{thread_width, thread_height, 1};
    const MTL::Size threads_per_grid{width, height, 1};
    encoder->dispatchThreads(threads_per_grid, threads_per_group);
  }

  static void DispatchSampleThreads(MTL::ComputeCommandEncoder* encoder,
                                    MTL::ComputePipelineState* pipeline, uint32_t width,
                                    uint32_t height, uint32_t sample_count) {
    const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
    const auto thread_height =
        std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
    const MTL::Size threads_per_group{thread_width, thread_height, 1};
    const MTL::Size threads_per_grid{width, height, sample_count};
    encoder->dispatchThreads(threads_per_grid, threads_per_group);
  }

 private:
  static auto MakeBuffer(size_t length, MTL::ResourceOptions storage_mode, const char* label)
      -> NS::SharedPtr<MTL::Buffer> {
    auto* device = MetalContext::Instance().Device();
    if (device == nullptr) {
      throw std::runtime_error(std::string(Derived::kStageLabel) +
                               ": Metal device is unavailable.");
    }

    auto buffer =
        NS::TransferPtr(device->newBuffer(static_cast<NS::UInteger>(length), storage_mode));
    if (!buffer) {
      throw std::runtime_error(std::string(Derived::kStageLabel) + ": failed to allocate " + label +
                               " buffer.");
    }
    return buffer;
  }
};

}  // namespace alcedo::metal_detail

#endif  // HAVE_METAL
