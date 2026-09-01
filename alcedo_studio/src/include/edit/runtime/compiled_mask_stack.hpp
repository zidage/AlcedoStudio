//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/**
 * @brief Graph value for one Mask source after evaluate (and fused Brush feather).
 *
 * Producer is the owning Color Grade. The port is derived from @p mask_id, not a list
 * index.
 */
[[nodiscard]] inline auto MaskSourceValue(const NodeId& owner, const MaskId& mask_id)
    -> GraphValueId {
  return GraphValueId{owner, PortId{std::string{mask_id.Value()} + ".source"}};
}

/**
 * @brief Distinct Brush feather value. Fused evaluate currently writes the source port.
 */
[[nodiscard]] inline auto MaskFeatherValue(const NodeId& owner, const MaskId& mask_id)
    -> GraphValueId {
  return GraphValueId{owner, PortId{std::string{mask_id.Value()} + ".feather"}};
}

/**
 * @brief Per-Mask effective coverage after invert, range identity, and opacity.
 *
 * Fused with source evaluate until a later distinct effective pass exists.
 */
[[nodiscard]] inline auto MaskEffectiveValue(const NodeId& owner, const MaskId& mask_id)
    -> GraphValueId {
  return GraphValueId{owner, PortId{std::string{mask_id.Value()} + ".effective"}};
}

/**
 * @brief Union coverage for every enabled Mask on Color Grade @p owner.
 */
[[nodiscard]] inline auto MaskUnionValue(const NodeId& owner) -> GraphValueId {
  return GraphValueId{owner, PortId{"mask.union"}};
}

/**
 * @brief Signed-distance scratch owned by one Brush Mask. Distinct per @p mask_id.
 */
[[nodiscard]] inline auto MaskSignedDistanceValue(const NodeId& owner, const MaskId& mask_id)
    -> GraphValueId {
  return GraphValueId{owner, PortId{std::string{mask_id.Value()} + ".signed_distance"}};
}

/**
 * @brief Per-Mask scratch value (horizontal/inside/outside distance planes).
 */
[[nodiscard]] inline auto MaskScratchValue(const NodeId& owner, const MaskId& mask_id,
                                           std::string_view suffix) -> GraphValueId {
  return GraphValueId{owner, PortId{std::string{mask_id.Value()} + "." + std::string{suffix}}};
}

/**
 * @brief One compiled Mask source. Runtime reads enabled/opacity/invert from the live model.
 *
 * @p source_kind and @p mask_id are static structure. @p range_input is the owning
 * Grade's scene input, never that Grade's output.
 */
struct CompiledMaskSource {
  NodeId         owner_node_id;
  MaskId         mask_id;
  MaskSourceKind source_kind = MaskSourceKind::Radial;
  GraphValueId   source_output{NodeId{""}, PortId{""}};
  GraphValueId   feather_output{NodeId{""}, PortId{""}};
  GraphValueId   effective_output{NodeId{""}, PortId{""}};
  GraphValueId   range_input{NodeId{""}, PortId{"image"}};
};

/**
 * @brief Compiled Mask list for one Color Grade. Sources are sorted by @ref MaskId.
 *
 * Display order lives only on the model. Empty lists omit this stack. A nonempty
 * all-disabled list still has sources and a Union output (zero coverage).
 */
struct CompiledMaskStack {
  NodeId                          owner_node_id;
  std::vector<CompiledMaskSource> sources;
  GraphValueId                    union_output{NodeId{""}, PortId{"mask.union"}};

  /**
   * @brief Compiled source with @p id, or null.
   */
  [[nodiscard]] auto FindSource(const MaskId& id) const -> const CompiledMaskSource* {
    for (const auto& source : sources) {
      if (source.mask_id == id) {
        return &source;
      }
    }
    return nullptr;
  }
};

}  // namespace alcedo
