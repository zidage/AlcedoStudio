//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>

#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaMaskResult {
  GraphValueId  output;
  std::uint64_t persistent_texture_resource_id = 0;
  std::uint64_t active_texture_resource_id     = 0;
  std::uint64_t signed_distance_resource_id    = 0;
  std::uint32_t mip_level_count                = 0;
};

/**
 * @brief Evaluate @p compiled_source into its effective GraphValueId (RenderSpace R8).
 *
 * Persistent Brush textures are keyed by MaskAssetKey and are never patched. Active Brush
 * pixels use a separate session-generation texture and dirty-rectangle upload. Changing
 * only feather radius reuses signed distance when the raster bytes are unchanged. No CPU
 * image processing fallback is used.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade,
                                   const CompiledMaskSource& compiled_source,
                                   MaskStore* store = nullptr,
                                   std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> CudaMaskResult;

/**
 * @brief Maximum-Union enabled Mask sources into the Grade Union output.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source texture.
 * Two or more fold a native R8 maximum. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteCudaMaskUnion(CudaRenderDevice& device, const ExecutionPlan& plan,
                                        const PipelineDocument& document,
                                        const CompiledGradeNode& compiled_grade) -> CudaMaskResult;

/**
 * @brief Evaluate every enabled source on @p compiled_grade and Union into mask_output.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade,
                                   MaskStore* store = nullptr,
                                   std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> CudaMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document, MaskStore* store = nullptr,
                                   std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> CudaMaskResult;

}  // namespace alcedo
