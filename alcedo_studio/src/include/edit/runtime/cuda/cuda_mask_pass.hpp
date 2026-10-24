//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/mask_coverage_texture_cache.hpp"

namespace alcedo {

struct CudaMaskResult {
  GraphValueId output;
};

/**
 * @brief Evaluate @p compiled_source into its effective GraphValueId (RenderSpace R8).
 *
 * Analytic sources evaluate in the native kernel. Invert and opacity apply in
 * that order. No CPU image processing fallback is used.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade,
                                   const CompiledMaskSource& compiled_source) -> CudaMaskResult;

/**
 * @brief Maximum-Union enabled Mask sources into the Grade Union output.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source texture.
 * Two or more fold a native R8 maximum over the full render extent so a lowered
 * source can decrease coverage. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteCudaMaskUnion(CudaRenderDevice& device, const ExecutionPlan& plan,
                                        const PipelineDocument& document,
                                        const CompiledGradeNode& compiled_grade) -> CudaMaskResult;

/**
 * @brief Evaluate every enabled source on @p compiled_grade and Union into mask_output.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade) -> CudaMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document) -> CudaMaskResult;

/**
 * @brief Record the coverage display write of @p coverage (SM8.5): the raster sampling of the
 *        committed AI MaskEvaluate on the photo frame grid of @p request, with its invert and
 *        opacity, into @p output (RGBA32F; red is the R8 value / 255, green and blue 0, alpha 1).
 *
 * @pre @p output has the render extent of @p request.geometry; the device stream orders the
 *      coverage writes before this dispatch.
 * @throws std::runtime_error for a wrong output or a failed launch.
 */
void               RecordMaskCoverageDisplay(CudaRenderDevice&                     device,
                                             const MaskCoverageImage<CudaBackend>& coverage,
                                             const MaskCoverageDisplayRequest&     request,
                                             CudaBackend::Texture2D&               output);

}  // namespace alcedo
