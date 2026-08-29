//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClPrimaryGradeResult {
  GraphValueId  output{NodeId{"grade.primary"}, PortId{"image"}};
  std::uint32_t pointwise_dispatch_count               = 0;
  std::uint32_t detail_pass_count                      = 0;
  std::uint32_t local_tone_pass_count                  = 0;
  std::uint32_t local_tone_transient_bytes             = 0;
  std::uint32_t command_upload_bytes                   = 0;
  std::uint64_t lut_resource_id                        = 0;
  std::uint64_t local_tone_reference_resource_id       = 0;
  bool          local_tone_rebuilt_reference           = false;
  bool          local_tone_sampled_canonical_reference = false;
};

/**
 * @brief Encode the shared Primary Grade adjustment order on the OpenCL queue.
 *
 * Pointwise adjustments are packed into fused dispatches, neighborhood work is
 * a separate workspace texture pass, and local-tone stages build or sample the
 * canonical workspace reference. Parameters are stored in ParameterArena and
 * failures are reported directly.
 */
[[nodiscard]] auto ExecuteOpenClPrimaryGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                             const PreparedRawInput& prepared,
                                             PipelineDocument&       document)
    -> OpenClPrimaryGradeResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
