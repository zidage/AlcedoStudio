//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief Decode the ACEScc AP1 @p scene to linear AP1 and apply the diffusion filter (phase
 *        VK12): the Vulkan form of `ExecuteOpenClDiffusionFilter`.
 *
 * Writes the scene-work member that @ref DestinationWorkMember selects for @p scene. With strength
 * 0 the pass only decodes. With a positive strength a render that covers the full edit space
 * builds the scatter image on the canvas of @ref DiffusionCanvasExtent and publishes it as
 * @ref DiffusionScatterId; a region of interest samples the published image at the reference
 * position of each pixel. Without a current published image (or when the render may not persist
 * results) the render builds a scatter image of its own pixels. The pyramid levels are images of
 * the texture pool, given back before the pass returns (each command of the recording waits for
 * the previous one). Every dispatch records into the open recording; the pass submits nothing.
 *
 * @return The scene-work binding that holds linear AP1.
 * @throws std::runtime_error when rendering has not begun, the DRT node is missing, the scene-work
 *         member has another extent than @p scene, or a dispatch fails; nothing is dispatched for
 *         the first three.
 */
[[nodiscard]] auto ExecuteVulkanDiffusionFilter(VulkanRenderDevice&      device,
                                                const ExecutionPlan&     plan,
                                                const PipelineDocument&  document,
                                                const FrameSceneBinding& scene)
    -> FrameSceneBinding;

}  // namespace alcedo

#endif  // HAVE_VULKAN
