//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaDrtResult {
  GraphValueId output{NodeId{"drt"}, PortId{"display"}};
};

/**
 * @brief Encode the selected ACES 2.0 or OpenDRT display transform on CUDA.
 *
 * The input must be the compiled primary-grade AP1 image. The device owns resolved ACES tables
 * and reuses them until DRT parameters change. A failed parameter upload retains Model dirty bits.
 * No CPU image-processing fallback is attempted.
 */
[[nodiscard]] auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan,
                                  PipelineDocument& document) -> CudaDrtResult;

}  // namespace alcedo
