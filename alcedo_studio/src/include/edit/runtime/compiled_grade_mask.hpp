//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/parameterized_brush_replay_cache.hpp"
#include "edit/mask/parameterized_brush_raster.hpp"
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

/**
 * @brief True when native Brush evaluation must replay parameterized strokes.
 *
 * Persistent MaskStore assets are used only when the Brush has an asset key and
 * no parameterized payload. Empty parameterized Brushes replay as zeros.
 */
inline auto BrushUsesParameterizedReplay(const BrushMaskSource& brush) -> bool {
  return BrushSourceHasParameterizedPayload(brush) || !brush.asset_key.has_value() ||
         brush.asset_key->Empty();
}

/**
 * @brief Request-owned canonical Brush raster tagged with the live Mask revision.
 *
 * Replays through the workspace-retained @p replay cache so an unchanged or
 * locally grown source does not re-rasterize the full canvas. Used by native
 * Mask passes when the PipelineApplyRequest has no active raster override.
 * Throws when rasterization fails.
 */
inline auto ParameterizedBrushActiveRasterForGrade(const PipelineDocument& document,
                                                   const NodeId& grade_id, const MaskId& mask_id,
                                                   const BrushMaskSource& brush,
                                                   Extent2D full_reference,
                                                   ParameterizedBrushReplayCache& replay)
    -> ActiveRasterMaskInput {
  const auto* grade =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(grade_id));
  const auto revision = grade == nullptr ? 1 : grade->MaskContentRevision(mask_id);
  return replay.Replay(grade_id, mask_id, brush, full_reference, revision);
}

}  // namespace alcedo
