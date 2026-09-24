//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_xtrans_interpolate.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

struct XTransParams {
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t passes;
  uint32_t green_radius;
  uint32_t rb_radius;
  uint32_t rgb_fc[36];
};

enum class Kernel : uint32_t {
  Green,
  Rgba,
};

auto KernelNameFor(Kernel kernel) -> const char* {
  switch (kernel) {
    case Kernel::Green:
      return "xtrans_green";
    case Kernel::Rgba:
      return "xtrans_rgba";
  }

  throw std::runtime_error("Metal X-Trans interpolate: unknown kernel.");
}

auto GetPipelineState(Kernel kernel) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH
  throw std::runtime_error("Metal X-Trans interpolate metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH, KernelNameFor(kernel),
      "Metal X-Trans interpolate");
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     uint32_t width, uint32_t height) {
  const auto thread_width  = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_group{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{width, height, 1};
  encoder->dispatchThreads(threads_per_grid, threads_per_group);
}

}  // namespace

void XTransToRGB_Ref(MetalImage& image, const XTransPattern6x6& pattern, int passes) {
  if (image.Empty()) {
    throw std::runtime_error("Metal X-Trans interpolate: input image is empty.");
  }
  if (image.Format() != PixelFormat::R32FLOAT) {
    throw std::runtime_error("Metal X-Trans interpolate: expected R32FLOAT raw input.");
  }

  const uint32_t width  = image.Width();
  const uint32_t height = image.Height();
  if (width == 0 || height == 0) {
    return;
  }

  MetalImage green =
      MetalImage::Create2D(width, height, PixelFormat::R32FLOAT, true, true, false);
  MetalImage output =
      MetalImage::Create2D(width, height, PixelFormat::RGBA32FLOAT, true, true, false);

  XTransParams params = {};
  params.width        = width;
  params.height       = height;
  params.tile_width   = 6;
  params.tile_height  = 6;
  params.passes       = static_cast<uint32_t>(std::max(passes, 1));
  params.green_radius = 3;
  params.rb_radius    = params.passes > 1 ? 4U : 3U;
  for (int i = 0; i < 36; ++i) {
    params.rgb_fc[i] = static_cast<uint32_t>(pattern.rgb_fc[i]);
  }

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal X-Trans interpolate: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal X-Trans interpolate: failed to create command buffer.");
  }

  {
    auto pipeline = GetPipelineState(Kernel::Green);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(image.Texture(), 0);
    compute->setTexture(green.Texture(), 1);
    compute->setBytes(&params, sizeof(params), 0);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::Rgba);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(image.Texture(), 0);
    compute->setTexture(green.Texture(), 1);
    compute->setTexture(output.Texture(), 2);
    compute->setBytes(&params, sizeof(params), 0);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
  image = std::move(output);
}

}  // namespace metal
}  // namespace alcedo

#endif
