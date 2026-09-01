//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <span>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

struct MetalMaskResult {
  GraphValueId  output;
  std::uint64_t persistent_texture_resource_id = 0;
  std::uint64_t active_texture_resource_id     = 0;
  std::uint64_t signed_distance_resource_id    = 0;
  std::uint32_t mip_level_count                = 0;
  std::uint32_t transient_bytes                = 0;
};

/**
 * @brief Evaluate @p compiled_source into its effective GraphValueId (RenderSpace R8).
 *
 * Raster source textures and mip levels live in workspace MaskTextureCache or the separate
 * active-raster cache. Persistent assets are never patched. Signed-distance intermediates are
 * destroyed after the recorded command buffer completes. The signed-distance result is stored
 * by Mask content key so a feather-radius edit can reuse it. Feather (when present),
 * invert, and opacity run in that order. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument& document,
                                    const CompiledGradeNode& compiled_grade,
                                    const CompiledMaskSource& compiled_source,
                                    MaskStore* store = nullptr,
                                    std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> MetalMaskResult;

/**
 * @brief Maximum-Union enabled Mask sources into the Grade Union output.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source texture.
 * Two or more fold a native R8 maximum over the full render extent so an erasing Brush
 * dirty update can decrease coverage. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteMetalMaskUnion(MetalRenderDevice& device, const ExecutionPlan& plan,
                                         const PipelineDocument& document,
                                         const CompiledGradeNode& compiled_grade)
    -> MetalMaskResult;

/**
 * @brief Evaluate every enabled source on @p compiled_grade and Union into mask_output.
 */
[[nodiscard]] auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument& document,
                                    const CompiledGradeNode& compiled_grade,
                                    MaskStore* store = nullptr,
                                    std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> MetalMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument& document, MaskStore* store = nullptr,
                                    std::span<const ActiveRasterMaskInput> active_raster_masks = {})
    -> MetalMaskResult;

void AppendMetalMaskWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
