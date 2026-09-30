//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClDrtResult {
  GraphValueId  output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId  display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run ACES 2.0 or OpenDRT, then display-referred DRT/Post operations.
 *
 * Consumes the linear AP1 output of the DiffusionFilter pass, transforms it to display-referred
 * values,
 * then applies neighborhood operations to the workspace RGBA32F display image.
 */
[[nodiscard]] auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                    const PipelineDocument&  document,
                                    const FrameSceneBinding& scene) -> OpenClDrtResult;

/**
 * @brief DiffusionFilter pass: decode the ACEScc AP1 @p scene to linear AP1.
 *
 * Writes the scene-work member that @ref DestinationWorkMember selects for @p scene.
 *
 * @return The work-image binding that holds linear AP1.
 * @throws std::runtime_error when the diffusion filter strength is not 0; the OpenCL scatter
 *         kernels do not exist yet. No other backend or substitute runs.
 */
[[nodiscard]] auto ExecuteOpenClDiffusionFilter(OpenClRenderDevice&      device,
                                                const PipelineDocument&  document,
                                                const FrameSceneBinding& scene)
    -> FrameSceneBinding;

}  // namespace alcedo

#endif  // HAVE_OPENCL
