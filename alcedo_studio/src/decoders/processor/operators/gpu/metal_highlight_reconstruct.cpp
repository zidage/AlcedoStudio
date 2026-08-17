//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_highlight_reconstruct.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

constexpr float    kHilightMagic     = 0.987f;
constexpr float    kChromaRingLo     = 0.2f;
constexpr uint32_t kBlockX           = 16;
constexpr uint32_t kBlockY           = 16;
constexpr size_t   kStatsFloats      = 6;

auto ChannelRatio(const float value, const float green) -> float {
  if (!std::isfinite(value) || value <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(value / green, 0.25f, 4.0f);
}

struct HighlightParams {
  float    clips[4];
  float    clipdark[4];
  uint32_t width;
  uint32_t height;
};

enum class Kernel : uint32_t {
  AccumulateStats,
  Reconstruct,
};

auto KernelNameFor(Kernel kernel) -> const char* {
  switch (kernel) {
    case Kernel::AccumulateStats:
      return "hlr_accumulate_stats";
    case Kernel::Reconstruct:
      return "hlr_reconstruct_tex";
  }
  throw std::runtime_error("Metal HighlightReconstruct: unknown kernel.");
}

auto GetPipelineState(Kernel kernel) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH
  throw std::runtime_error("Metal HighlightReconstruct metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH, KernelNameFor(kernel),
      "Metal HighlightReconstruct");
#endif
}

void DispatchImage(MTL::ComputeCommandEncoder* encoder, uint32_t width, uint32_t height) {
  const MTL::Size threads_per_group{kBlockX, kBlockY, 1};
  const MTL::Size threads_per_grid{width, height, 1};
  encoder->dispatchThreads(threads_per_grid, threads_per_group);
}

auto ScratchOutput() -> MetalImage& {
  static MetalImage output;
  return output;
}

auto StatsBuffer() -> NS::SharedPtr<MTL::Buffer>& {
  static NS::SharedPtr<MTL::Buffer> buffer;
  return buffer;
}

auto EnsureStatsBuffer() -> MTL::Buffer* {
  auto& buffer = StatsBuffer();
  if (buffer && buffer->length() >= kStatsFloats * sizeof(float)) {
    return buffer.get();
  }
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("Metal HighlightReconstruct: Metal device is unavailable.");
  }
  buffer = NS::TransferPtr(device->newBuffer(kStatsFloats * sizeof(float),
                                             MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("Metal HighlightReconstruct: failed to allocate stats buffer.");
  }
  return buffer.get();
}

}  // namespace

void HighlightReconstruct(MetalImage& img, LibRaw& raw_processor) {
  if (img.Empty()) {
    throw std::runtime_error("Metal HighlightReconstruct: input image is empty.");
  }
  if (img.Format() != PixelFormat::RGBA32FLOAT) {
    throw std::runtime_error("Metal HighlightReconstruct: expected RGBA32FLOAT RGB input.");
  }

  const uint32_t width  = img.Width();
  const uint32_t height = img.Height();
  if (width == 0 || height == 0) {
    return;
  }

  HighlightParams params = {};
  const float*    cam_mul = raw_processor.imgdata.color.cam_mul;
  const float green = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0f ? cam_mul[1] : 1.0f;
  params.clips[0]   = kHilightMagic * ChannelRatio(cam_mul[0], green);
  params.clips[1]   = kHilightMagic;
  params.clips[2]   = kHilightMagic * ChannelRatio(cam_mul[2], green);
  params.clipdark[0] = kChromaRingLo * params.clips[0];
  params.clipdark[1] = kChromaRingLo * params.clips[1];
  params.clipdark[2] = kChromaRingLo * params.clips[2];
  params.width       = width;
  params.height      = height;

  auto* stats = EnsureStatsBuffer();
  std::memset(stats->contents(), 0, kStatsFloats * sizeof(float));

  auto& output = ScratchOutput();
  output.Create(width, height, PixelFormat::RGBA32FLOAT, true, true, false);

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal HighlightReconstruct: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal HighlightReconstruct: failed to create command buffer.");
  }

  {
    auto pipeline = GetPipelineState(Kernel::AccumulateStats);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(img.Texture(), 0);
    compute->setBuffer(stats, 0, 0);
    compute->setBytes(&params, sizeof(params), 1);
    DispatchImage(compute.get(), width, height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::Reconstruct);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(img.Texture(), 0);
    compute->setTexture(output.Texture(), 1);
    compute->setBuffer(stats, 0, 0);
    compute->setBytes(&params, sizeof(params), 1);
    DispatchImage(compute.get(), width, height);
    compute->endEncoding();
  }

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
  img.Swap(output);
}

}  // namespace metal
}  // namespace alcedo

#endif
