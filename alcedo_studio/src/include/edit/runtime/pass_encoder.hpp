//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/pass_kind.hpp"

namespace alcedo {

class MaskStore;

/**
 * @brief Backend encode entry for one compiled pass kind.
 *
 * Specializations perform the GPU work. The primary template fails explicitly;
 * it does not select another backend, CPU, or a lower-quality path.
 *
 * @tparam Backend Render backend.
 * @tparam Kind Compiled pass kind.
 */
template <class Backend, GpuPassKind Kind>
struct PassEncoder {
  /**
   * @brief Encode @p Kind for @p Backend.
   * @throws std::runtime_error when no backend specialization exists.
   */
  template <class Device>
  static void Encode(Device&, const ExecutionPlan&, const PreparedRawInput&, PipelineDocument&,
                     MaskStore*) {
    throw std::runtime_error(std::string("PassEncoder: no specialization for ") +
                             GpuPassKindName(Kind));
  }
};

}  // namespace alcedo
