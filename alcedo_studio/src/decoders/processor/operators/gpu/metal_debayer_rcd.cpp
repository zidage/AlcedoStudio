//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_debayer_rcd.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

struct SinglePlaneParams {
  uint32_t width;
  uint32_t height;
  uint32_t rgb_fc[4];
};

enum class Kernel : uint32_t {
  InitAndVH,
  GreenAtRB,
  PQDir,
  RBAtRB,
  RBAtG,
  MergeRGBA,
};

constexpr uint32_t kRcdOutputCropRadius = 4;

auto KernelNameFor(Kernel kernel) -> const char* {
  switch (kernel) {
    case Kernel::InitAndVH:
      return "rcd_init_and_vh";
    case Kernel::GreenAtRB:
      return "rcd_green_at_rb";
    case Kernel::PQDir:
      return "rcd_pq_dir";
    case Kernel::RBAtRB:
      return "rcd_rb_at_rb";
    case Kernel::RBAtG:
      return "rcd_rb_at_g";
    case Kernel::MergeRGBA:
      return "rcd_merge_rgba";
  }

  throw std::runtime_error("Metal Debayer RCD: unknown kernel.");
}

auto GetPipelineState(Kernel kernel) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH
  throw std::runtime_error("Metal Debayer RCD metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH, KernelNameFor(kernel), "Metal Debayer RCD");
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     uint32_t width, uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_threadgroup{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{width, height, 1};
  encoder->dispatchThreads(threads_per_grid, threads_per_threadgroup);
}

void EnsurePlane(MetalImage& image, uint32_t width, uint32_t height) {
  image.Create(width, height, PixelFormat::R32FLOAT, true, true, false);
}

struct RcdScratch {
  MetalImage r;
  MetalImage g;
  MetalImage b;
  MetalImage vh;
  MetalImage pq;
};

auto Scratch() -> RcdScratch& {
  static RcdScratch scratch;
  return scratch;
}

}  // namespace

void Bayer2x2ToRGB_RCD(MetalImage& image, const BayerPattern2x2& pattern) {
  if (image.Empty()) {
    throw std::runtime_error("Metal Debayer RCD: input image is empty.");
  }
  if (image.Format() != PixelFormat::R32FLOAT) {
    throw std::runtime_error("Metal Debayer RCD: expected R32FLOAT Bayer input.");
  }

  const uint32_t in_width  = image.Width();
  const uint32_t in_height = image.Height();
  if (in_width == 0 || in_height == 0) {
    return;
  }
  if (in_width <= 2 * kRcdOutputCropRadius || in_height <= 2 * kRcdOutputCropRadius) {
    throw std::runtime_error("Metal Debayer RCD: image too small for RCD radius.");
  }
  const uint32_t out_width  = in_width - 2 * kRcdOutputCropRadius;
  const uint32_t out_height = in_height - 2 * kRcdOutputCropRadius;

  auto& scratch = Scratch();
  EnsurePlane(scratch.r, in_width, in_height);
  EnsurePlane(scratch.g, in_width, in_height);
  EnsurePlane(scratch.b, in_width, in_height);
  EnsurePlane(scratch.vh, in_width, in_height);
  EnsurePlane(scratch.pq, in_width, in_height);

  MetalImage output =
      MetalImage::Create2D(out_width, out_height, PixelFormat::RGBA32FLOAT, true, true, false);

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal Debayer RCD: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal Debayer RCD: failed to create command buffer.");
  }

  const SinglePlaneParams plane_params{
      .width  = in_width,
      .height = in_height,
      .rgb_fc = {static_cast<uint32_t>(pattern.rgb_fc[0]),
                 static_cast<uint32_t>(pattern.rgb_fc[1]),
                 static_cast<uint32_t>(pattern.rgb_fc[2]),
                 static_cast<uint32_t>(pattern.rgb_fc[3])},
  };

  {
    auto pipeline = GetPipelineState(Kernel::InitAndVH);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(image.Texture(), 0);
    compute->setTexture(scratch.r.Texture(), 1);
    compute->setTexture(scratch.g.Texture(), 2);
    compute->setTexture(scratch.b.Texture(), 3);
    compute->setTexture(scratch.vh.Texture(), 4);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), in_width, in_height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::GreenAtRB);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(image.Texture(), 0);
    compute->setTexture(scratch.vh.Texture(), 1);
    compute->setTexture(scratch.g.Texture(), 2);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), in_width, in_height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::PQDir);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(image.Texture(), 0);
    compute->setTexture(scratch.pq.Texture(), 1);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), in_width, in_height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::RBAtRB);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(scratch.pq.Texture(), 0);
    compute->setTexture(scratch.g.Texture(), 1);
    compute->setTexture(scratch.r.Texture(), 2);
    compute->setTexture(scratch.b.Texture(), 3);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), in_width, in_height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::RBAtG);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(scratch.vh.Texture(), 0);
    compute->setTexture(scratch.g.Texture(), 1);
    compute->setTexture(scratch.r.Texture(), 2);
    compute->setTexture(scratch.b.Texture(), 3);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), in_width, in_height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::MergeRGBA);
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(scratch.r.Texture(), 0);
    compute->setTexture(scratch.g.Texture(), 1);
    compute->setTexture(scratch.b.Texture(), 2);
    compute->setTexture(output.Texture(), 3);
    compute->setBytes(&plane_params, sizeof(plane_params), 0);
    DispatchThreads(compute.get(), pipeline.get(), out_width, out_height);
    compute->endEncoding();
  }

  command_buffer->commit();
  command_buffer->waitUntilCompleted();

  image = std::move(output);
}

}  // namespace metal
}  // namespace alcedo

#endif
