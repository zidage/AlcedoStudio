//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/// The outcome of one DRT encode on Vulkan: the fields of `OpenClDrtResult` and the comparable
/// host decisions of the shared executor.
struct VulkanDrtResult {
  GraphValueId         output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId         display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t        post_neighborhood_count = 0;
  DrtPostDecisionTrace trace;
};

/**
 * @brief Run ACES 2.0 or OpenDRT, then the display-referred DRT finish (phase VK12): the Vulkan
 *        form of `ExecuteOpenClDrt`, through the shared `DrtPostExecutor`.
 *
 * Reads the linear AP1 output of the DiffusionFilter pass (@p scene) and writes the RGBA32F
 * display output of @p plan. The DRT parameters (`PackVulkanDrtParams`, 11 096 bytes) are the
 * slot `drt.output` of the parameter arena; the slot is packed again only when the DRT revision
 * changes or the frame overrides the output color. Clarity, Sharpen, and Halation of the finish
 * run the neighborhood kernels of phase VK9 (`VulkanNeighborOps`). Every dispatch records into the
 * open recording; the pass submits nothing.
 *
 * @throws std::runtime_error before any command for a render that has not begun, a missing DRT, an
 *         output encoding that does not resolve, the film print (phase VK13), and an enabled Film
 *         Grain in the finish (phase VK14); for a failed dispatch after the commands before it.
 */
[[nodiscard]] auto ExecuteVulkanDrt(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument&  document,
                                    const FrameSceneBinding& scene) -> VulkanDrtResult;

}  // namespace alcedo

#endif  // HAVE_VULKAN
