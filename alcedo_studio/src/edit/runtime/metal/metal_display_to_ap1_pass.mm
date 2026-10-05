//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_display_to_ap1_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include <alcedo/metal/Metal.hpp>

#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {

auto MetalDisplayToAp1Parameters::Upload(MetalBackend&          backend,
                                         MetalCommandContext&   command_context,
                                         std::span<const float> packed)
    -> const MetalBackend::Buffer& {
  if (packed.empty()) {
    throw std::runtime_error("Metal DisplayToAp1: empty parameter block");
  }
  if (!buffer_.Empty() && uploaded_.size() == packed.size() &&
      std::equal(packed.begin(), packed.end(), uploaded_.begin())) {
    return buffer_;
  }
  if (buffer_.Empty() || buffer_.Bytes() < packed.size_bytes()) {
    buffer_ = backend.CreateBuffer(packed.size_bytes());
  }
  backend.UploadBufferRange(buffer_, 0, std::as_bytes(packed), command_context);
  uploaded_.assign(packed.begin(), packed.end());
  ++upload_count_;
  return buffer_;
}

void EncodeMetalDisplayToAp1(MetalBackend& backend, MetalCommandContext& command_context,
                             const MetalBackend::Texture2D& src, MetalBackend::Texture2D& dst,
                             const MetalBackend::Buffer& params, DisplayToAp1Output output_kind) {
#ifndef ALCEDO_METAL_DISPLAY_TO_AP1_METALLIB_PATH
  (void)backend;
  (void)command_context;
  (void)src;
  (void)dst;
  (void)params;
  (void)output_kind;
  throw std::runtime_error("Metal DisplayToAp1 metallib path is not configured.");
#else
  const char* kernel_name = output_kind == DisplayToAp1Output::LinearAp0 ? "display_to_ap0_linear"
                                                                         : "display_to_ap1_acescc";
  auto        pipeline    = metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DISPLAY_TO_AP1_METALLIB_PATH, kernel_name, "Metal DisplayToAp1");
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      backend.EnsureComputeCommandEncoder(command_context));
  if (encoder == nullptr) {
    throw std::runtime_error("EncodeMetalDisplayToAp1: compute encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBuffer(static_cast<MTL::Buffer*>(params.Native()), 0, 0);
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{src.Width(), src.Height(), 1},
                           MTL::Size{thread_width, thread_height, 1});
  backend.NoteComputeDispatch(command_context);
#endif
}

}  // namespace alcedo
