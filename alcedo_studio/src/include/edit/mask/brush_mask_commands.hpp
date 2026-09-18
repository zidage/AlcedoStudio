//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>

#include "edit/geometry/types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_id.hpp"

namespace alcedo {

/**
 * @brief Append one validated stroke to an existing Brush on a Color Grade.
 *
 * @p expected_revision is the Mask content revision observed before this
 * operation. Admission is not a history commit.
 */
struct AppendBrushStrokeCommand {
  NodeId      node_id;
  MaskId      mask_id;
  BrushStroke stroke;
  std::uint64_t expected_revision = 0;
};

/**
 * @brief Remove one stroke by StrokeId. Inverse of append/insert at the owner.
 *
 * Does not copy remaining sample bodies. @p expected_revision must match the
 * Mask content revision before the removal.
 */
struct RemoveBrushStrokeCommand {
  NodeId   node_id;
  MaskId   mask_id;
  StrokeId stroke_id;
  std::uint64_t expected_revision = 0;
};

/**
 * @brief Insert a stroke at a display/evaluation index on an existing Brush.
 *
 * @p index past the end appends. Duplicate StrokeId is rejected. The existing
 * stroke list is left unchanged on failure.
 */
struct InsertBrushStrokeCommand {
  NodeId      node_id;
  MaskId      mask_id;
  std::size_t index = 0;
  BrushStroke stroke;
  std::uint64_t expected_revision = 0;
};

/**
 * @brief Replace Brush placement_translation with an exact after value.
 *
 * @p before must equal the current translation. The operation does not rewrite
 * or copy canonical samples. Unchanged after equal to current is a no-op.
 */
struct SetBrushTranslationCommand {
  NodeId  node_id;
  MaskId  mask_id;
  Vector2 before{};
  Vector2 after{};
  std::uint64_t expected_revision = 0;
};

}  // namespace alcedo
