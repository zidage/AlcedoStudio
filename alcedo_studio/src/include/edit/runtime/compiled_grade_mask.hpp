//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Resolve the Grade-owned Mask named by a compiled Color Grade.
 *
 * @pre @p compiled.mask is present.
 * @throws std::runtime_error when the Grade or MaskId is missing.
 */
inline auto RequireCompiledMaskModel(const PipelineDocument& document,
                                     const CompiledGradeNode& compiled) -> const MaskModel& {
  if (!compiled.mask.has_value()) {
    throw std::runtime_error("compiled Color Grade has no mask");
  }
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(compiled.node_id));
  if (grade == nullptr) {
    throw std::runtime_error("compiled Color Grade is missing from the document");
  }
  const auto* mask = grade->FindMask(compiled.mask->mask_id);
  if (mask == nullptr) {
    throw std::runtime_error("compiled MaskId '" + std::string{compiled.mask->mask_id.Value()} +
                             "' is missing from the Color Grade");
  }
  return *mask;
}

}  // namespace alcedo
