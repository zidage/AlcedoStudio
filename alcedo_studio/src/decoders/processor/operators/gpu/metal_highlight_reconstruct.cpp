//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_highlight_reconstruct.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

constexpr float kHilightMagic = 0.987f;
// Lower bound of the chrominance sampling ring, as a fraction of the clip level. Matches
// current darktable opposed. Darker samples inside the dilated ring sit on the shadow side of
// highlight boundaries and only inject large negative outliers into the global statistic.
constexpr float kChromaRingLo = 0.2f;
// Minimum ring sample count before the global chrominance is trusted. darktable uses >100 on
// mosaiced and >30 on demosaiced data; this operator runs post-demosaic.
constexpr float kMinChromaSamples = 30.0f;
constexpr int   kMaskPlanes       = 6;

// WB multipliers come from file metadata or the camera's as-shot estimate and can be zero,
// negative, or non-finite on exotic files. Clamp the per-channel ratio to a sane band so a
// broken multiplier can neither invert the clip order nor push a clip level far outside the
// sensor's physical range.
auto ChannelRatio(const float value, const float green) -> float {
  if (!std::isfinite(value) || value <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(value / green, 0.25f, 4.0f);
}

struct HighlightCorrectionParams {
  float    clips[4];
  float    clipdark[4];
  float    chrominance[4];
  uint32_t width;
  uint32_t height;
  uint32_t stride;
};

enum class Kernel : uint32_t {
  BuildMask,
  DilateMask,
  ChrominanceContrib,
  Reconstruct,
};

constexpr uint32_t kRowAlignmentBytes = 256;

auto AlignRowBytes(size_t row_bytes) -> size_t {
  return ((row_bytes + kRowAlignmentBytes - 1) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

auto KernelNameFor(Kernel kernel) -> const char* {
  switch (kernel) {
    case Kernel::BuildMask:
      return "hlr_build_mask";
    case Kernel::DilateMask:
      return "hlr_dilate_mask";
    case Kernel::ChrominanceContrib:
      return "hlr_chrominance_contrib";
    case Kernel::Reconstruct:
      return "hlr_reconstruct";
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

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("Metal HighlightReconstruct: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("Metal HighlightReconstruct: failed to allocate shared buffer.");
  }

  return buffer;
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

  const auto row_bytes   = AlignRowBytes(static_cast<size_t>(width) * sizeof(float) * 4U);
  const auto buffer_size = row_bytes * height;
  const auto stride      = static_cast<uint32_t>(row_bytes / (sizeof(float) * 4U));
  const auto size        = static_cast<size_t>(width) * height;
  const auto mask_size   = static_cast<size_t>(kMaskPlanes) * size * sizeof(uint8_t);

  HighlightCorrectionParams params = {};
  const float*              cam_mul = raw_processor.imgdata.color.cam_mul;
  const float green = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0f ? cam_mul[1] : 1.0f;
  params.clips[0]   = kHilightMagic * ChannelRatio(cam_mul[0], green);
  params.clips[1]   = kHilightMagic;
  params.clips[2]   = kHilightMagic * ChannelRatio(cam_mul[2], green);
  params.clipdark[0] = kChromaRingLo * params.clips[0];
  params.clipdark[1] = kChromaRingLo * params.clips[1];
  params.clipdark[2] = kChromaRingLo * params.clips[2];
  params.width       = width;
  params.height      = height;
  params.stride      = stride;

  auto input_buffer        = MakeSharedBuffer(buffer_size);
  auto mask_buffer         = MakeSharedBuffer(mask_size);
  auto dilated_mask_buffer = MakeSharedBuffer(mask_size);
  auto contrib_buffer      = MakeSharedBuffer(buffer_size);
  auto count_buffer        = MakeSharedBuffer(buffer_size);
  auto anyclipped_buffer   = MakeSharedBuffer(sizeof(uint32_t));

  std::memset(mask_buffer->contents(), 0, mask_size);
  std::memset(dilated_mask_buffer->contents(), 0, mask_size);
  std::memset(contrib_buffer->contents(), 0, buffer_size);
  std::memset(count_buffer->contents(), 0, buffer_size);
  std::memset(anyclipped_buffer->contents(), 0, sizeof(uint32_t));

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal HighlightReconstruct: Metal queue is unavailable.");
  }

  auto mask_command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!mask_command_buffer) {
    throw std::runtime_error("Metal HighlightReconstruct: failed to create command buffer.");
  }

  {
    auto blit = NS::RetainPtr(mask_command_buffer->blitCommandEncoder());
    blit->copyFromTexture(img.Texture(), 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{width, height, 1},
                          input_buffer.get(), 0, row_bytes, buffer_size);
    blit->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::BuildMask);
    auto compute  = NS::RetainPtr(mask_command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(input_buffer.get(), 0, 0);
    compute->setBuffer(mask_buffer.get(), 0, 1);
    compute->setBuffer(anyclipped_buffer.get(), 0, 2);
    compute->setBytes(&params, sizeof(params), 3);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::DilateMask);
    auto compute  = NS::RetainPtr(mask_command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(mask_buffer.get(), 0, 0);
    compute->setBuffer(dilated_mask_buffer.get(), 0, 1);
    compute->setBytes(&params, sizeof(params), 2);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  {
    auto pipeline = GetPipelineState(Kernel::ChrominanceContrib);
    auto compute  = NS::RetainPtr(mask_command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(input_buffer.get(), 0, 0);
    compute->setBuffer(dilated_mask_buffer.get(), 0, 1);
    compute->setBuffer(contrib_buffer.get(), 0, 2);
    compute->setBuffer(count_buffer.get(), 0, 3);
    compute->setBytes(&params, sizeof(params), 4);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  mask_command_buffer->commit();
  mask_command_buffer->waitUntilCompleted();

  const auto anyclipped = *static_cast<const uint32_t*>(anyclipped_buffer->contents());
  if (anyclipped == 0U) {
    return;
  }

  std::array<float, 4> sums = {0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> cnts = {0.0f, 0.0f, 0.0f, 0.0f};

  const auto* contrib_data = static_cast<const float*>(contrib_buffer->contents());
  const auto* count_data   = static_cast<const float*>(count_buffer->contents());
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t col = 0; col < width; ++col) {
      const size_t pixel_index = (static_cast<size_t>(row) * stride + col) * 4U;
      for (int c = 0; c < 3; ++c) {
        sums[c] += contrib_data[pixel_index + static_cast<size_t>(c)];
        cnts[c] += count_data[pixel_index + static_cast<size_t>(c)];
      }
    }
  }

  for (int c = 0; c < 3; ++c) {
    params.chrominance[c] = (cnts[c] > kMinChromaSamples) ? (sums[c] / cnts[c]) : 0.0f;
  }

  auto output_buffer = MakeSharedBuffer(buffer_size);

  auto reconstruct_command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!reconstruct_command_buffer) {
    throw std::runtime_error("Metal HighlightReconstruct: failed to create command buffer.");
  }

  {
    auto pipeline = GetPipelineState(Kernel::Reconstruct);
    auto compute  = NS::RetainPtr(reconstruct_command_buffer->computeCommandEncoder());
    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(input_buffer.get(), 0, 0);
    compute->setBuffer(output_buffer.get(), 0, 1);
    compute->setBytes(&params, sizeof(params), 2);
    DispatchThreads(compute.get(), pipeline.get(), width, height);
    compute->endEncoding();
  }

  {
    auto blit = NS::RetainPtr(reconstruct_command_buffer->blitCommandEncoder());
    blit->copyFromBuffer(output_buffer.get(), 0, row_bytes, buffer_size,
                         MTL::Size{width, height, 1}, img.Texture(), 0, 0,
                         MTL::Origin{0, 0, 0});
    blit->endEncoding();
  }

  reconstruct_command_buffer->commit();
  reconstruct_command_buffer->waitUntilCompleted();
}

};  // namespace metal
};  // namespace alcedo

#endif
