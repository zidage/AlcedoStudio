//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "decoders/processor/operators/gpu/metal_to_linear_ref.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "decoders/processor/operators/gpu/metal_encode.hpp"
#include "decoders/processor/raw_normalization.hpp"
#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace metal {
namespace {

struct WBParams {
  float    black_level[4];
  float    white_level[4];
  float    wb_multipliers[4];
  uint32_t apply_white_balance;
  uint32_t padding[3];
};

struct ToLinearRefParams {
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
  uint32_t black_tile_width;
  uint32_t black_tile_height;
  uint32_t raw_fc[36];
};

auto GetPipelineState() -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH
  throw std::runtime_error("Metal ToLinearRef metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH, "to_linear_ref_r16u", "Metal ToLinearRef");
#endif
}

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("[FATAL] Metal ToLinearRef: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("[FATAL] Metal ToLinearRef: failed to allocate staging buffer.");
  }

  return buffer;
}

void DispatchToLinearRef(const MetalImage& src, MetalImage& dst, const WBParams& wb_params,
                         const RawCfaPattern& pattern, const std::vector<float>& black_pattern,
                         uint32_t black_tile_width, uint32_t black_tile_height) {
  auto black_pattern_buffer =
      MakeSharedBuffer(std::max<size_t>(black_pattern.size(), 1) * sizeof(float));
  std::fill_n(static_cast<float*>(black_pattern_buffer->contents()),
              std::max<size_t>(black_pattern.size(), 1), 0.0f);
  if (!black_pattern.empty()) {
    std::copy(black_pattern.begin(), black_pattern.end(),
              static_cast<float*>(black_pattern_buffer->contents()));
  }

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("[ERROR] Metal ToLinearRef: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("[ERROR] Metal ToLinearRef: failed to create command buffer.");
  }

  auto pipeline = GetPipelineState();
  auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

  ToLinearRefParams params = {};
  params.width             = src.Width();
  params.height            = src.Height();
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    params.tile_width  = 6;
    params.tile_height = 6;
    for (int i = 0; i < 36; ++i) {
      params.raw_fc[i] = static_cast<uint32_t>(pattern.xtrans_pattern.raw_fc[i]);
    }
  } else {
    params.tile_width  = 2;
    params.tile_height = 2;
    for (int i = 0; i < 4; ++i) {
      params.raw_fc[i] = static_cast<uint32_t>(pattern.bayer_pattern.raw_fc[i]);
    }
  }
  params.black_tile_width  = black_tile_width;
  params.black_tile_height = black_tile_height;

  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_threadgroup{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{src.Width(), src.Height(), 1};

  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(src.Texture(), 0);
  compute->setTexture(dst.Texture(), 1);
  compute->setBytes(&params, sizeof(params), 0);
  compute->setBytes(&wb_params, sizeof(wb_params), 1);
  compute->setBuffer(black_pattern_buffer.get(), 0, 2);
  compute->dispatchThreads(threads_per_grid, threads_per_threadgroup);
  compute->endEncoding();

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

static auto GetWBCoeff(const libraw_rawdata_t& raw_data) -> const float* {
  return raw_data.color.cam_mul;
}

static auto GetPatternBlackLevels(const libraw_rawdata_t& raw_data) -> std::vector<float> {
  const uint32_t tile_width  = raw_data.color.cblack[4];
  const uint32_t tile_height = raw_data.color.cblack[5];
  const uint32_t entries     = tile_width * tile_height;
  if (entries == 0U) {
    return {};
  }

  std::vector<float> pattern_black(entries, 0.0f);
  for (uint32_t i = 0; i < entries; ++i) {
    pattern_black[i] = static_cast<float>(raw_data.color.cblack[6 + i]);
  }
  return pattern_black;
}

}  // namespace

void ToLinearRef(MetalImage& img, LibRaw& raw_processor, const RawCfaPattern& pattern) {
  const auto raw_curve     = raw_norm::BuildLinearizationCurve(raw_processor.imgdata.rawdata);
  const auto wb            = GetWBCoeff(raw_processor.imgdata.rawdata);
  auto       black_pattern = GetPatternBlackLevels(raw_processor.imgdata.rawdata);
  const uint32_t black_tile_width  = raw_processor.imgdata.rawdata.color.cblack[4];
  const uint32_t black_tile_height = raw_processor.imgdata.rawdata.color.cblack[5];

  if (img.Format() != metal::PixelFormat::R16UINT) {
    throw std::runtime_error("Metal ToLinearRef: expected R16UINT raw input.");
  }

  MetalImage linearized =
      MetalImage::Create2D(img.Width(), img.Height(), PixelFormat::R32FLOAT, true, true, false);

  WBParams wb_params = {};
  for (int c = 0; c < 4; ++c) {
    wb_params.black_level[c]    = raw_curve.black_level[c];
    wb_params.white_level[c]    = raw_curve.white_level[c];
    wb_params.wb_multipliers[c] = wb[c];
  }
  wb_params.apply_white_balance =
      (raw_processor.imgdata.color.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) == 0 ? 1u : 0u;

  DispatchToLinearRef(img, linearized, wb_params, pattern, black_pattern, black_tile_width,
                      black_tile_height);
  img = std::move(linearized);
}

void LinearizeRgb(metal::MetalImage& img, const RawRgbLinearizationParams& params) {
  if (img.Format() != PixelFormat::RGBA32FLOAT) {
    throw std::runtime_error("Metal RGB: expected F32 RGBA");
  }
  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) throw std::runtime_error("Metal RGB: queue is unavailable");
  auto buffer = NS::RetainPtr(queue->commandBuffer());
  if (!buffer) throw std::runtime_error("Metal RGB: command buffer creation failed");
  EncodeLinearizeRgb(buffer.get(), img.Texture(), params);
  buffer->commit();
  buffer->waitUntilCompleted();
  if (buffer->status() == MTL::CommandBufferStatusError) {
    throw std::runtime_error("Metal RGB: GPU linearization failed");
  }
}

};  // namespace metal
};  // namespace alcedo

#endif
