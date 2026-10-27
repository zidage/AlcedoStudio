//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/mask_coverage_texture_cache.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClMaskResult {
  GraphValueId output;
};

/**
 * @brief Evaluate @p compiled_source into its effective GraphValueId (RenderSpace R8).
 *
 * Analytic sources evaluate in the native kernel. Invert, the Luminance Range and Hue Range
 * weights (MaskRangeParams), and opacity apply in that order. The function only enqueues OpenCL
 * work; failures throw and no CPU or other-backend substitute is used.
 *
 * @param range_scene The owning Grade's input image (ACEScc AP1, RGBA32F on the render grid),
 *        never that Grade's output. Only a Mask with a restricted range reads it.
 * @throws std::runtime_error when a Mask with a restricted range has no RGBA32F @p range_scene of
 *         the render extent.
 */
[[nodiscard]] auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument&   document,
                                     const CompiledGradeNode&  compiled_grade,
                                     const CompiledMaskSource& compiled_source,
                                     const FrameSceneBinding&  range_scene) -> OpenClMaskResult;

/**
 * @brief Maximum-Union enabled Mask sources into the Grade Union output.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source texture.
 * Two or more fold a native R8 maximum over the full render extent so a lowered
 * source can decrease coverage. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteOpenClMaskUnion(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                          const PipelineDocument& document,
                                          const CompiledGradeNode& compiled_grade)
    -> OpenClMaskResult;

/**
 * @brief Evaluate every enabled source on @p compiled_grade and Union into mask_output.
 *
 * A Mask range reads `CompiledMaskSource::range_input` as a cached image, so a range on a Grade
 * after the first one needs the per-source overload with the running scene binding.
 */
[[nodiscard]] auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument& document,
                                     const CompiledGradeNode& compiled_grade)
    -> OpenClMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument& document) -> OpenClMaskResult;

/**
 * @brief Record the coverage display write of @p coverage (SM8.5): the raster sampling of the
 *        committed AI MaskEvaluate on the photo frame grid of @p request, with its invert and
 *        opacity, into @p output (RGBA32F; red is the R8 value / 255, green and blue 0, alpha 1).
 *
 * Enqueues on the device queue and tracks the event in the command context.
 * @throws std::runtime_error for a wrong output, a kernel lookup, or an enqueue failure.
 */
void               RecordMaskCoverageDisplay(OpenClRenderDevice&                     device,
                                             const MaskCoverageImage<OpenClBackend>& coverage,
                                             const MaskCoverageDisplayRequest&       request,
                                             OpenClBackend::Texture2D&               output);

/**
 * @brief Record the display write of a probed Mask (Mask ranges plan D8): the R8 coverage
 *        @p mask, which MaskEvaluate wrote on the render grid, into @p output on the same grid
 *        (RGBA32F; red is the R8 value / 255, green and blue 0, alpha 1). No resampling.
 *
 * Enqueues on the device queue and tracks the event in the command context.
 * @throws std::runtime_error when @p mask is not R8, @p output is not RGBA32F of the same size,
 *         or the kernel lookup or enqueue fails.
 */
void RecordMaskValueDisplay(OpenClRenderDevice& device, const OpenClBackend::Texture2D& mask,
                            OpenClBackend::Texture2D& output);

}  // namespace alcedo

#endif  // HAVE_OPENCL
