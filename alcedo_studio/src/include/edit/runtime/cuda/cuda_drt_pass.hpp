//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaDrtResult {
  GraphValueId  output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId  display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run the selected display transform, then display-referred DRT/Post operations.
 *
 * The display kernel decodes the compiled ACEScc input before ACES 2.0 or OpenDRT. Neighborhood
 * operations consume that display-referred result and write the final display output. Grade mix
 * and masks do not suppress these endpoint operations. A failed parameter upload retains Model
 * dirty bits.
 */
[[nodiscard]] auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan,
                                  PipelineDocument& document) -> CudaDrtResult;

}  // namespace alcedo
