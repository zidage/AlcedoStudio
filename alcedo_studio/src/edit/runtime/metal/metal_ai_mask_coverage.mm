//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM8.4: Metal dispatch of the AI Mask Feather and Expand (SM8 section 5.1). The kernels are in
// shader/mask.metal (ai_mask_morphology, ai_mask_feather_rows, ai_mask_feather_columns).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <alcedo/metal/Metal.hpp>

#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

/// Layout of `AiMaskEdgeGrid` in mask.metal.
struct AiMaskEdgeGrid {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t source_x      = 0;
  std::int32_t source_y      = 0;
  std::int32_t source_width  = 0;
  std::int32_t source_height = 0;
};

static_assert(sizeof(AiMaskEdgeGrid) == 24);

/// Layout of `AiMaskFeatherWeights` in mask.metal; bound with setBytes (below the 4 KiB limit).
struct FeatherWeightsArgument {
  std::int32_t radius = 0;
  float        values[kAiMaskFeatherMaximumRadius + 1]{};
};

static_assert(sizeof(FeatherWeightsArgument) == 808);

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_MASK_METALLIB_PATH
  (void)function;
  (void)label;
  throw std::runtime_error("MetalBackend::RecordAiMaskEdge: the Mask metallib path is not set");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(ALCEDO_METAL_MASK_METALLIB_PATH,
                                                                  function, label);
#endif
}

void RequireTexture(const MetalBackend::Texture2D* texture, Extent2D extent, TextureFormat format,
                    const char* name) {
  if (texture == nullptr || texture->Native() == nullptr) {
    throw std::runtime_error(std::string{"MetalBackend::RecordAiMaskEdge: the "} + name +
                             " texture is missing");
  }
  if (texture->Width() != extent.width || texture->Height() != extent.height ||
      texture->Format() != format) {
    throw std::runtime_error(std::string{"MetalBackend::RecordAiMaskEdge: the "} + name +
                             " texture is not " + std::to_string(extent.width) + " x " +
                             std::to_string(extent.height) + " " + TextureFormatName(format));
  }
}

/// Grid of a pass whose input is @p input placed at (@p x, @p y) in the output.
auto MakeGrid(Extent2D output, const MetalBackend::Texture2D& input, std::int32_t x, std::int32_t y)
    -> AiMaskEdgeGrid {
  return AiMaskEdgeGrid{
      static_cast<std::int32_t>(output.width),  static_cast<std::int32_t>(output.height), x, y,
      static_cast<std::int32_t>(input.Width()), static_cast<std::int32_t>(input.Height())};
}

auto NativeTexture(const MetalBackend::Texture2D& texture) -> MTL::Texture* {
  return static_cast<MTL::Texture*>(texture.Native());
}

void Dispatch2D(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                Extent2D extent) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{extent.width, extent.height, 1},
                           MTL::Size{thread_width, thread_height, 1});
}

}  // namespace

void MetalBackend::RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                                    const AiMaskEdgeTextures<Texture2D>& textures,
                                    CommandContext&                      command_context) {
  const auto extent = dispatch.output_extent;
  if (extent.Empty() || (dispatch.expand_steps == 0 && !dispatch.HasFeather())) {
    throw std::runtime_error("MetalBackend::RecordAiMaskEdge: the dispatch has no work");
  }
  if (static_cast<int>(dispatch.feather_weights.size()) > kAiMaskFeatherMaximumRadius + 1) {
    throw std::runtime_error("MetalBackend::RecordAiMaskEdge: the Feather radius is too large");
  }
  RequireTexture(textures.source, dispatch.source_extent, TextureFormat::R8, "source");
  RequireTexture(textures.output, extent, TextureFormat::R8, "output");
  for (std::uint32_t i = 0; i < dispatch.MorphologyWorkTextureCount(); ++i) {
    RequireTexture(textures.morphology[i], extent, TextureFormat::R8, "morphology work");
  }
  if (dispatch.HasFeather()) {
    RequireTexture(textures.feather_rows, extent, TextureFormat::R32f, "Feather work");
  }

  auto* encoder =
      static_cast<MTL::ComputeCommandEncoder*>(EnsureComputeCommandEncoder(command_context));
  if (encoder == nullptr) {
    throw std::runtime_error("MetalBackend::RecordAiMaskEdge: the compute encoder is missing");
  }

  const Texture2D* current = textures.source;
  AiMaskEdgeGrid   grid    = MakeGrid(extent, *current, dispatch.source_x, dispatch.source_y);
  if (dispatch.expand_steps > 0) {
    const auto pipeline = Pipeline("ai_mask_morphology", "Metal AI Mask morphology");
    for (std::int32_t step = 0; step < dispatch.expand_steps; ++step) {
      const bool last_writes_output = !dispatch.HasFeather() && step + 1 == dispatch.expand_steps;
      Texture2D& target = last_writes_output ? *textures.output : *textures.morphology[step % 2];
      const std::int32_t operation[2] = {dispatch.dilate ? 1 : 0, step % 2};
      encoder->setComputePipelineState(pipeline.get());
      encoder->setTexture(NativeTexture(*current), 0);
      encoder->setTexture(NativeTexture(target), 1);
      encoder->setBytes(&grid, sizeof(grid), 0);
      encoder->setBytes(operation, sizeof(operation), 1);
      Dispatch2D(encoder, pipeline.get(), extent);
      NoteComputeDispatch(command_context);
      current = &target;
      grid    = MakeGrid(extent, target, 0, 0);
    }
  }

  if (dispatch.HasFeather()) {
    FeatherWeightsArgument weights;
    weights.radius = static_cast<std::int32_t>(dispatch.feather_weights.size()) - 1;
    std::copy(dispatch.feather_weights.begin(), dispatch.feather_weights.end(), weights.values);

    const auto rows_pipeline = Pipeline("ai_mask_feather_rows", "Metal AI Mask Feather rows");
    encoder->setComputePipelineState(rows_pipeline.get());
    encoder->setTexture(NativeTexture(*current), 0);
    encoder->setTexture(NativeTexture(*textures.feather_rows), 1);
    encoder->setBytes(&grid, sizeof(grid), 0);
    encoder->setBytes(&weights, sizeof(weights), 1);
    Dispatch2D(encoder, rows_pipeline.get(), extent);
    NoteComputeDispatch(command_context);

    const auto columns_pipeline =
        Pipeline("ai_mask_feather_columns", "Metal AI Mask Feather columns");
    const auto columns_grid = MakeGrid(extent, *textures.feather_rows, 0, 0);
    encoder->setComputePipelineState(columns_pipeline.get());
    encoder->setTexture(NativeTexture(*textures.feather_rows), 0);
    encoder->setTexture(NativeTexture(*textures.output), 1);
    encoder->setBytes(&columns_grid, sizeof(columns_grid), 0);
    encoder->setBytes(&weights, sizeof(weights), 1);
    Dispatch2D(encoder, columns_pipeline.get(), extent);
    NoteComputeDispatch(command_context);
  }
}

}  // namespace alcedo
