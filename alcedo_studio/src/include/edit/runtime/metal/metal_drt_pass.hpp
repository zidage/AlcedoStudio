//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

struct MetalDrtResult {
  GraphValueId  output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId  scene_post{NodeId{"drt"}, PortId{"runtime.scene_post"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run DRT/Post neighborhood ops in ACEScc, then ACES 2.0 or OpenDRT.
 *
 * Input is the compiled primary-grade AP1/ACEScc image. Neighborhood ops write
 * @ref ExecutionPlan::drt_scene_output. The display kernel then decodes ACEScc
 * to AP1 scene-linear. Grade mix and masks do not suppress these operations.
 */
[[nodiscard]] auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                                   PipelineDocument& document) -> MetalDrtResult;

void               AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
