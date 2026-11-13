//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/ai_mask_asset_references.hpp"

#include <string>
#include <vector>

namespace alcedo {

void CollectAiMaskAssetKeys(const nlohmann::json& value, std::set<MaskAssetKey>& keys) {
  // Explicit stack: history batches nest documents several levels deep, and a deep JSON value
  // must not overflow the call stack.
  std::vector<const nlohmann::json*> pending{&value};
  while (!pending.empty()) {
    const auto* current = pending.back();
    pending.pop_back();
    if (current->is_object()) {
      const auto kind = current->find("kind");
      const auto key  = current->find("asset_key");
      if (kind != current->end() && *kind == "ai_segmentation" && key != current->end() &&
          key->is_string()) {
        keys.insert(MaskAssetKey{key->get<std::string>()});
      }
    }
    if (current->is_structured()) {
      for (const auto& child : *current) {
        if (child.is_structured()) {
          pending.push_back(&child);
        }
      }
    }
  }
}

}  // namespace alcedo
