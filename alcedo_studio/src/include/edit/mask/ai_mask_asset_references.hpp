//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <set>

#include "edit/mask/mask_asset.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Add the asset key of every AI Segmentation Mask source in @p value to @p keys.
 *
 * Walks objects and arrays recursively. An object with `"kind": "ai_segmentation"` and a string
 * `asset_key` contributes its key. The walk has no rule per document or batch type, so it finds
 * the keys in documents, checkpoints, roots, and every history batch (`AddMask`, `RemoveMask`,
 * `ReplaceMaskSource`, node graph changes, Paste). Strings that hold JSON are not parsed.
 *
 * @param value Any JSON value; it is not changed.
 * @param keys Receives the keys; existing entries stay.
 */
void CollectAiMaskAssetKeys(const nlohmann::json& value, std::set<MaskAssetKey>& keys);

}  // namespace alcedo
