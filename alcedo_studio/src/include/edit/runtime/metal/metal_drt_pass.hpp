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
  GraphValueId  display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run ACES 2.0 or OpenDRT, then display-referred DRT/Post operations.
 *
 * Input is the compiled DRT scene-input ACEScc image. The display transform runs first, and
 * neighborhood operations consume its display-referred result.
 */
[[nodiscard]] auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                                   PipelineDocument& document) -> MetalDrtResult;

void               AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
