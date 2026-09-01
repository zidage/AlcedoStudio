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
  GraphValueId  scene_post{NodeId{"drt"}, PortId{"runtime.scene_post"}};
  std::uint32_t post_neighborhood_count = 0;
};

/**
 * @brief Run DRT/Post neighborhood ops in ACEScc, then ACES 2.0 or OpenDRT.
 *
 * Consumes the compiled DRT scene-input ACEScc image and writes a workspace RGBA32F display
 * image. Grade mix and masks do not suppress the endpoint operations.
 */
[[nodiscard]] auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                    PipelineDocument& document) -> OpenClDrtResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
