//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_pass_encoder.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/plan_executor.hpp"

namespace alcedo {

auto CudaRenderDevice::Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                               PipelineDocument& document, bool publish_on_success,
                               TransientAllocationPolicy transient_policy,
                               ResultPersistenceScope persistence)
    -> GraphValueId {
  return PlanExecutor<CudaBackend>::Execute(*this, plan, input, document, publish_on_success,
                                            transient_policy, persistence);
}

}  // namespace alcedo
