//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/mask_coverage_texture_cache.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/// The R8 output of one Mask pass on Vulkan: the form of `OpenClMaskResult`.
struct VulkanMaskResult {
  GraphValueId output;
};

/**
 * @brief MaskEvaluate on Vulkan (phase VK11): evaluate @p compiled_source into its effective
 * GraphValueId (RenderSpace R8), the form of the OpenCL overload of the same arguments.
 *
 * A Radial or a Linear Gradient Mask evaluates in `mask_analytic_r8`; an AI Mask samples its
 * coverage texture (`AcquireAiMaskCoverage`, after its Feather and Expand) in `mask_raster_r8`.
 * Invert, the Luminance Range and Hue Range weights (MaskRangeParams), and opacity apply in that
 * order; the value is quantized to R8. Records into the open recording; does not submit or wait.
 *
 * @param range_scene The owning Grade's input image (ACEScc AP1, RGBA32F on the render grid: a
 *        cached image or a scene-work member), never that Grade's output. Only a Mask with a
 *        restricted range reads it.
 * @throws std::runtime_error "ExecuteVulkanMask: ..." outside `BeginRender`, for empty geometry
 *         extents, a compiled Mask that does not match the document, or a Mask with a restricted
 *         range whose @p range_scene is not an RGBA32F image of the render extent; the errors of
 *         the coverage acquisition (a missing asset) propagate. The caller cancels the render.
 */
[[nodiscard]] auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument&   document,
                                     const CompiledGradeNode&  compiled_grade,
                                     const CompiledMaskSource& compiled_source,
                                     const FrameSceneBinding&  range_scene) -> VulkanMaskResult;

/**
 * @brief MaskUnion on Vulkan: the maximum of the enabled Mask sources of @p compiled_grade into
 * its Union output, the form of `ExecuteOpenClMaskUnion`.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source image. Two or more copy
 * the first source into the Union output and fold the others in place with `mask_union_max_r8`,
 * over the full render extent, so a lowered source can decrease coverage. Records into the open
 * recording; does not submit or wait.
 *
 * @throws std::runtime_error "ExecuteVulkanMaskUnion: ..." outside `BeginRender`, for a Grade
 *         without a Mask stack, or a missing enabled source image.
 */
[[nodiscard]] auto ExecuteVulkanMaskUnion(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                          const PipelineDocument&  document,
                                          const CompiledGradeNode& compiled_grade)
    -> VulkanMaskResult;

/**
 * @brief Every enabled source of @p compiled_grade, then the Union into its mask output.
 *
 * A Mask range reads `CompiledMaskSource::range_input` as a cached image, so a range on a Grade
 * after the first one needs the per-source overload with the running scene binding.
 */
[[nodiscard]] auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument&  document,
                                     const CompiledGradeNode& compiled_grade) -> VulkanMaskResult;

/**
 * @brief Every compiled Color Grade Mask stack in backbone order.
 *
 * @return The result of the last Grade with a Mask stack.
 * @throws std::runtime_error when no compiled Grade has a Mask stack.
 */
[[nodiscard]] auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument& document) -> VulkanMaskResult;

/**
 * @brief Records the coverage display write of @p coverage (SM8.5): the raster sampling of the
 * committed AI MaskEvaluate on the photo frame grid of @p request, with its invert and opacity,
 * into @p output (RGBA32F; red is the R8 value / 255, green and blue 0, alpha 1).
 *
 * @throws std::runtime_error when @p output is not an RGBA32F image of the photo frame size.
 */
void               RecordMaskCoverageDisplay(VulkanRenderDevice&                     device,
                                             const MaskCoverageImage<VulkanBackend>& coverage,
                                             const MaskCoverageDisplayRequest&       request,
                                             VulkanBackend::Texture2D&               output);

/**
 * @brief Records the range probe display of a Mask that is not in the plan (#604), such as the
 * candidate of a new AI Mask: the raster MaskEvaluate of @p coverage on the render grid of
 * @p plan with the invert, Luminance Range, Hue Range, and opacity of @p mask, into @p output
 * (RGBA32F; red is the value that `ExecuteVulkanMask` writes for the same Mask).
 *
 * @param range_scene The input image of the probed Grade; only a restricted range reads it.
 * @throws std::runtime_error for an output that is not an RGBA32F image of the render extent, or
 *         a wrong Grade input.
 */
void RecordMaskCandidateRangeDisplay(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                     const MaskCoverageImage<VulkanBackend>& coverage,
                                     const MaskModel& mask, const FrameSceneBinding& range_scene,
                                     VulkanBackend::Texture2D& output);

/**
 * @brief Records the display write of a probed Mask (Mask ranges plan D8): the R8 coverage
 * @p mask on the render grid into @p output on the same grid (RGBA32F; red is the R8 value / 255,
 * green and blue 0, alpha 1). No resampling.
 *
 * @throws std::runtime_error when @p mask is not R8 or @p output is not an RGBA32F image of the
 *         same size.
 */
void RecordMaskValueDisplay(VulkanRenderDevice& device, const VulkanBackend::Texture2D& mask,
                            VulkanBackend::Texture2D& output);

}  // namespace alcedo

#endif  // HAVE_VULKAN
