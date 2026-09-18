//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/brush_mask_commands.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

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
 * @brief Default source feather for a newly created Brush: 0.5% of the shorter
 * full-reference edge, in reference pixels. Existing Brushes keep their stored
 * feather; this value is applied only at first-stroke creation.
 */
[[nodiscard]] inline auto DefaultBrushFeatherReferencePixels(Extent2D full_reference) -> float {
  const auto shorter = (std::min)(full_reference.width, full_reference.height);
  if (shorter == 0) {
    return 0.0f;
  }
  return 0.005f * static_cast<float>(shorter);
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

/**
 * @brief True when every dab in @p stroke carries no positive strength.
 *
 * A Paint stroke with only zero-strength samples cannot change coverage, so a
 * settle may skip its history commit.
 */
[[nodiscard]] inline auto BrushStrokeHasCoverageEffect(const BrushStroke& stroke) -> bool {
  if (stroke.samples == nullptr) {
    return false;
  }
  for (const auto& sample : *stroke.samples) {
    if (sample.strength > 0.0f) {
      return true;
    }
  }
  return false;
}

/**
 * @brief True when @p erase can overlap positive Paint coverage in @p committed.
 *
 * Both sides are compared in world reference pixels (local plus the current
 * placement translation). A disc test on dab centers and radii is
 * conservative: overlapping erase/paint discs may still erase nothing when
 * earlier Erase strokes already removed the coverage there. That is a valid
 * commit; the no-change skip only needs the disjoint case.
 */
[[nodiscard]] inline auto BrushEraseOverlapsPaint(const BrushStroke& erase,
                                                  const BrushMaskSource& committed) -> bool {
  if (erase.samples == nullptr) {
    return false;
  }
  const auto overlap = [&erase](const BrushStroke& paint) {
    if (paint.mode != BrushStrokeMode::Paint || paint.samples == nullptr) {
      return false;
    }
    for (const auto& e : *erase.samples) {
      if (!(e.strength > 0.0f)) {
        continue;
      }
      for (const auto& p : *paint.samples) {
        if (!(p.strength > 0.0f)) {
          continue;
        }
        const float dx       = e.local_x - p.local_x;
        const float dy       = e.local_y - p.local_y;
        const float reach    = e.radius + p.radius;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < reach) {
          return true;
        }
      }
    }
    return false;
  };
  for (const auto& paint : committed.strokes) {
    if (overlap(paint)) {
      return true;
    }
  }
  return false;
}

}  // namespace alcedo
