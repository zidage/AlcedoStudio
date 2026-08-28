//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClDrtResult {
  GraphValueId output{NodeId{"drt"}, PortId{"display"}};
};

/**
 * @brief Encode ACES 2.0 or OpenDRT on the current OpenCL product queue.
 *
 * The pass consumes the primary-grade ACEScc image and writes a workspace
 * RGBA32F display image. It does not wait or read pixels back.
 */
[[nodiscard]] auto ExecuteOpenClDrt(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                    PipelineDocument& document) -> OpenClDrtResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
