//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

/**
 * @brief Decode the ACEScc AP1 @p scene to linear AP1 and apply the diffusion filter.
 *
 * Writes the scene-work member that @ref DestinationWorkMember selects for @p scene. With
 * strength 0 the pass only decodes. With a positive strength it reads the full-frame scatter
 * image of @ref DiffusionScatterId: a render that covers the full edit space builds it on the
 * canvas of @ref DiffusionCanvasExtent and publishes it; a viewport ROI render samples the
 * published image at the reference position of each pixel. Without a current published image
 * (or when the render may not persist results) the render builds a submission-local image from
 * its own pixels. Pyramid levels are pooled scratch textures released before the pass returns;
 * all work is ordered on the device command stream.
 *
 * @return The work-image binding that holds linear AP1.
 * @throws std::runtime_error when rendering has not begun, the DRT node is missing, or a
 *         kernel launch fails.
 */
[[nodiscard]] auto ExecuteCudaDiffusionFilter(CudaRenderDevice& device, const ExecutionPlan& plan,
                                              const PipelineDocument&  document,
                                              const FrameSceneBinding& scene) -> FrameSceneBinding;

}  // namespace alcedo
