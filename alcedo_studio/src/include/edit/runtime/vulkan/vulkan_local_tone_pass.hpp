//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/local_tone_plan.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/// The outcome of one Local Laplacian stage on Vulkan: the fields of `OpenClLocalToneResult` and
/// the decision of the shared executor.
struct VulkanLocalToneResult {
  std::uint64_t          reference_resource_id       = 0;
  bool                   rebuilt_reference           = false;
  bool                   sampled_canonical_reference = false;
  std::uint32_t          transient_bytes             = 0;
  LocalToneDecisionTrace trace;
};

/**
 * @brief The Shadows and Highlights stage of a Color Grade on Vulkan (phase VK10): the form of
 * `ExecuteOpenClLocalTone`, through the shared `LocalToneExecutor`.
 *
 * Builds the log intensity of @p adjusted into the mask plane (the canonical plane for a full
 * frame), its Gaussian pyramid, the pyramids of the remapped samples, the selected Laplacian
 * levels, and the collapse, then tones @p working by the planes and, when @p mix is not 1 or
 * @p mask_id is set, mixes it with @p original times the R8 mask. A valid canonical result is
 * sampled without a rebuild; a slider edit reuses the canonical source.
 *
 * Storage: the pyramid planes are ranges of the transient arena of the workspace, read by device
 * address; the canonical source and result planes are R32F images of the graph image cache
 * (`LocalToneSourceId`, `LocalToneResultId`), recorded unpublished: `PublishResults` publishes
 * them after the render, `CancelRender` drops them.
 *
 * Records into the open recording; does not submit or wait.
 *
 * @pre @p working is a scene-work member and equals @p adjusted (the OpenCL precondition).
 * @throws std::runtime_error "ExecuteVulkanLocalTone: ..." outside `BeginRender`, for empty
 *         geometry extents, a working binding that is not the adjusted scene-work member, a missing
 *         or wrong mask, scene images of different sizes, and lost canonical planes. The caller
 *         cancels the render.
 */
[[nodiscard]] auto ExecuteVulkanLocalTone(VulkanRenderDevice&      device,
                                          const FrameSceneBinding& adjusted,
                                          const FrameSceneBinding& working,
                                          const FrameSceneBinding& original, const NodeId& grade_id,
                                          float shadows_slider, float highlights_slider,
                                          const ResolvedRenderGeometry& geometry, float mix,
                                          const GraphValueId* mask_id) -> VulkanLocalToneResult;

}  // namespace alcedo

#endif  // HAVE_VULKAN
