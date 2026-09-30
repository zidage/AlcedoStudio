//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

struct CudaDrtResult {
  GraphValueId  output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId  display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run the selected display transform, then display-referred DRT/Post operations.
 *
 * The display kernel reads the linear AP1 output of the DiffusionFilter pass. Neighborhood
 * operations consume that display-referred result and write the final display output. Grade mix
 * and masks do not suppress these endpoint operations. Reads the document only; a failed
 * parameter upload stays queued in the workspace arena.
 */
[[nodiscard]] auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan,
                                  const PipelineDocument& document, const FrameSceneBinding& scene)
    -> CudaDrtResult;

}  // namespace alcedo
