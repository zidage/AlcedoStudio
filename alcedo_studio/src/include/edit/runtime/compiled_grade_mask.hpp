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
 * @brief Resolve one Grade-owned Mask by @p mask_id.
 *
 * @throws std::runtime_error when the Grade or MaskId is missing.
 */
inline auto RequireMaskModel(const PipelineDocument& document, const NodeId& grade_id,
                             const MaskId& mask_id) -> const MaskModel& {
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(grade_id));
  if (grade == nullptr) {
    throw std::runtime_error("compiled Color Grade is missing from the document");
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    throw std::runtime_error("compiled MaskId '" + std::string{mask_id.Value()} +
                             "' is missing from the Color Grade");
  }
  return *mask;
}

/**
 * @brief Live enabled flag for a compiled Mask source. Does not use cached plan fields.
 */
inline auto MaskSourceIsEnabled(const PipelineDocument& document, const NodeId& grade_id,
                                const MaskId& mask_id) -> bool {
  return RequireMaskModel(document, grade_id, mask_id).enabled;
}

/**
 * @brief Compiled Mask stack on @p compiled.
 *
 * @throws std::runtime_error when the Color Grade has no Mask stack.
 */
inline auto RequireMaskStack(const CompiledGradeNode& compiled) -> const CompiledMaskStack& {
  if (!compiled.mask_stack.has_value()) {
    throw std::runtime_error("compiled Color Grade has no Mask stack");
  }
  return *compiled.mask_stack;
}

}  // namespace alcedo
