//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_cvt_ref_space.hpp"

#include <algorithm>
#include <stdexcept>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

struct InverseCamMulParams {
  float scale_r;
  float scale_g;
  float scale_b;
  float scale_a;
};

constexpr float kMinGain = 1e-6f;

auto GetPipelineState() -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH
  throw std::runtime_error("Metal ApplyInverseCamMul metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH, "apply_inverse_cam_mul_rgba32f",
      "Metal ApplyInverseCamMul");
#endif
}

void DispatchInverseCamMul(MetalImage& image, const float* cam_mul) {
  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal ApplyInverseCamMul: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal ApplyInverseCamMul: failed to create command buffer.");
  }

  auto        pipeline = GetPipelineState();
  auto        compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());
  const float g        = std::max(cam_mul[1], kMinGain);
  const InverseCamMulParams params{
      .scale_r = g / std::max(cam_mul[0], kMinGain),
      .scale_g = 1.0f,
      .scale_b = g / std::max(cam_mul[2], kMinGain),
      .scale_a = 1.0f,
  };

  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_threadgroup{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{image.Width(), image.Height(), 1};

  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(image.Texture(), 0);
  compute->setBytes(&params, sizeof(params), 0);
  compute->dispatchThreads(threads_per_grid, threads_per_threadgroup);
  compute->endEncoding();

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

}  // namespace

void ApplyInverseCamMul(MetalImage& img, const float* cam_mul) {
  if (img.Empty()) {
    return;
  }
  if (img.Format() != PixelFormat::RGBA32FLOAT) {
    throw std::runtime_error("Metal ApplyInverseCamMul: expected RGBA32FLOAT image.");
  }
  if (cam_mul == nullptr) {
    throw std::runtime_error("Metal ApplyInverseCamMul: cam_mul is null.");
  }

  DispatchInverseCamMul(img, cam_mul);
}

}  // namespace metal
}  // namespace alcedo

#endif
