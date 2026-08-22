//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <exception>
#include <stdexcept>

#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

auto CudaRenderDevice::Execute(const ExecutionPlan& plan, const PreparedRawInput& input,
                               PipelineDocument& document, MaskStore* mask_store) -> GraphValueId {
  try {
    BeginRender();
    ExecuteCudaDevelop(*this, plan, input, document);
    if (plan.primary_grade_mask) {
      (void)ExecuteCudaMask(*this, plan, document, mask_store);
    }
    (void)ExecuteCudaPrimaryGrade(*this, plan, input.color_context, document);
    const auto result = ExecuteCudaDrt(*this, plan, document);
    EndRender();
    return result.output;
  } catch (const std::exception& ex) {
    CancelRender();
    ReportError(ex.what());
    throw;
  } catch (...) {
    CancelRender();
    ReportError("CUDA DAG execution failed with an unknown error");
    throw;
  }
}

}  // namespace alcedo
