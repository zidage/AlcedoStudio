//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/brush_mask_commands.hpp"
#include "edit/mask/mask_id.hpp"

namespace alcedo {

/**
 * @brief Initial dab radius: 2% diameter of the shorter full-reference edge.
 *
 * Strength and hardness stay at 1. Empty extents yield radius 1 so a later
 * validator can still reject a missing photograph instead of dividing by zero.
 */
[[nodiscard]] inline auto DefaultBrushRadiusReferencePixels(Extent2D full_reference) -> float {
  const auto shorter = (std::min)(full_reference.width, full_reference.height);
  if (shorter == 0) {
    return 1.0f;
  }
  return 0.01f * static_cast<float>(shorter);
}

/**
 * @brief Convert a world ReferenceSpace pixel to Brush-local coordinates.
 *
 * Samples are stored as @p reference minus @p placement_translation. Movement
 * changes only translation; it does not rewrite sample bodies.
 *
 * @param reference World reference pixels from the shared viewer mapping.
 * @param translation Current @c BrushMaskSource::placement_translation.
 */
[[nodiscard]] inline auto BrushLocalFromReference(Vector2 reference, Vector2 translation)
    -> Vector2 {
  return Vector2{reference.x - translation.x, reference.y - translation.y};
}

/**
 * @brief Convert a Brush-local sample coordinate to world reference pixels.
 */
[[nodiscard]] inline auto BrushWorldFromLocal(Vector2 local, Vector2 translation) -> Vector2 {
  return Vector2{local.x + translation.x, local.y + translation.y};
}

/**
 * @brief Exact after-value for a Brush move: @p before plus a reference-pixel delta.
 *
 * Does not resample or shift stored samples. A→B→A uses this twice with opposite
 * deltas and restores the original translation.
 */
[[nodiscard]] inline auto BrushPlacementAfterReferenceDelta(Vector2 before, Vector2 delta)
    -> Vector2 {
  return Vector2{before.x + delta.x, before.y + delta.y};
}

/**
 * @brief Placement after dragging from @p press_reference to @p current_reference.
 *
 * @p before is the translation captured at press. The drag delta is
 * `current_reference - press_reference` in world reference pixels, not item space.
 */
[[nodiscard]] inline auto BrushPlacementForReferenceDrag(Vector2 before, Vector2 press_reference,
                                                           Vector2 current_reference) -> Vector2 {
  return BrushPlacementAfterReferenceDelta(
      before, Vector2{current_reference.x - press_reference.x,
                       current_reference.y - press_reference.y});
}

/**
 * @brief Build an owner SetBrushTranslation command with exact before/after values.
 *
 * Admission is not a history commit. Equal before/after remains a no-op at the owner.
 */
[[nodiscard]] inline auto MakeBrushTranslationCommand(const NodeId& node_id, const MaskId& mask_id,
                                                     Vector2 before, Vector2 after,
                                                     std::uint64_t expected_revision)
    -> SetBrushTranslationCommand {
  SetBrushTranslationCommand command;
  command.node_id            = node_id;
  command.mask_id             = mask_id;
  command.before              = before;
  command.after               = after;
  command.expected_revision   = expected_revision;
  return command;
}

}  // namespace alcedo
