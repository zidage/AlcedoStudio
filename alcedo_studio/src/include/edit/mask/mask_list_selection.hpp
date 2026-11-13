//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <span>

#include "edit/mask/mask_id.hpp"

namespace alcedo {

/**
 * @brief Next selected Mask after deleting @p deleted from @p ordered_ids.
 *
 * Display order is the Grade Mask list. Deleting a Mask that is not selected
 * keeps @p selected. Deleting the selected Mask chooses the next row at the
 * same index, otherwise the previous row, otherwise none.
 *
 * Thread: any. Pure.
 */
[[nodiscard]] inline auto MaskIdAfterDeletion(std::span<const MaskId> ordered_ids,
                                                const MaskId& deleted, const MaskId& selected)
    -> MaskId {
  if (deleted.Empty() || deleted != selected) {
    return selected;
  }
  std::size_t index = ordered_ids.size();
  for (std::size_t i = 0; i < ordered_ids.size(); ++i) {
    if (ordered_ids[i] == deleted) {
      index = i;
      break;
    }
  }
  if (index >= ordered_ids.size()) {
    return {};
  }
  if (index + 1 < ordered_ids.size()) {
    return ordered_ids[index + 1];
  }
  if (index > 0) {
    return ordered_ids[index - 1];
  }
  return {};
}

/**
 * @brief Selection after Undo restores @p restored.
 *
 * Selects the restored Mask only when the current selection is empty. A valid
 * current selection is preserved.
 */
[[nodiscard]] inline auto MaskIdAfterUndoRestore(const MaskId& restored, const MaskId& selected)
    -> MaskId {
  if (selected.Empty()) {
    return restored;
  }
  return selected;
}

}  // namespace alcedo
