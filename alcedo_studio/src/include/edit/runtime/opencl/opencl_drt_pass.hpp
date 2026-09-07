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
 * Consumes the compiled DRT scene-input ACEScc image, transforms it to display-referred values,
 * then applies neighborhood operations to the workspace RGBA32F display image.
 */
[[nodiscard]] auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                    PipelineDocument& document) -> OpenClDrtResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
