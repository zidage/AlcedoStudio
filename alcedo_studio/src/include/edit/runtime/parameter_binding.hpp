//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"

namespace alcedo {

/**
 * @brief One field copy from a DTO payload into the ParameterArena.
 *
 * destination_offset is relative to the slot start when passed to BindSlot;
 * the arena stores the absolute arena offset after bind.
 */
struct ParameterFieldBinding {
  DirtyFieldMask dirty_bit;
  std::uint32_t  source_offset      = 0;
  std::uint32_t  destination_offset = 0;
  std::uint32_t  size               = 0;
};

/// Stable ParameterArena placement for one adjustment instance.
struct ParameterBinding {
  std::uint32_t                       offset = 0;
  std::uint32_t                       size   = 0;
  std::vector<ParameterFieldBinding>  fields;
};

/// Lookup key for a bound parameter slot.
struct ParameterSlotKey {
  NodeId               node_id;
  AdjustmentInstanceId adjustment_id;

  friend auto operator==(const ParameterSlotKey& lhs, const ParameterSlotKey& rhs) -> bool {
    return lhs.node_id == rhs.node_id && lhs.adjustment_id == rhs.adjustment_id;
  }
  friend auto operator<(const ParameterSlotKey& lhs, const ParameterSlotKey& rhs) -> bool {
    if (lhs.node_id != rhs.node_id) {
      return lhs.node_id < rhs.node_id;
    }
    return lhs.adjustment_id < rhs.adjustment_id;
  }
};

}  // namespace alcedo
