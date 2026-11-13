//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Decode the ACEScc AP1 @p scene to linear AP1 and apply the diffusion filter.
 *
 * The Metal counterpart of @ref ExecuteCudaDiffusionFilter; both backends run the same scatter
 * pyramid and publish the same full-frame scatter image of @ref DiffusionScatterId. Writes the
 * scene-work member that @ref DestinationWorkMember selects for @p scene. With strength 0 the
 * pass only decodes. With a positive strength a render that covers the full edit space builds
 * the scatter image on the canvas of @ref DiffusionCanvasExtent and publishes it; a viewport ROI
 * render samples the published image at the reference position of each pixel. Without a current
 * published image (or when the render may not persist results) the render builds a
 * submission-local image from its own pixels. Pyramid levels are pooled scratch textures released
 * before the pass returns; all dispatches are ordered in the frame's serial compute encoder.
 *
 * @return The work-image binding that holds linear AP1.
 * @throws std::runtime_error when rendering has not begun, the DRT node is missing, the DRT
 *         metallib is not configured, or the compute encoder is missing.
 */
[[nodiscard]] auto ExecuteMetalDiffusionFilter(MetalRenderDevice& device, const ExecutionPlan& plan,
                                               const PipelineDocument&  document,
                                               const FrameSceneBinding& scene) -> FrameSceneBinding;

void               AppendMetalDiffusionFilterWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif  // HAVE_METAL
