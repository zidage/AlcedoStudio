//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/**
 * @brief Render-side port that reads the stored R8 asset of an AI Mask.
 *
 * The assets live in the project database. Implementations: the project store
 * (`ProjectMaskAssetReader`), the store of the project that is open now (the editor executor), and
 * @ref DefaultMaskAssetReader for renderers without a project.
 *
 * Thread: @ref Read may be called from several render threads at the same time.
 */
class MaskAssetReader {
 public:
  virtual ~MaskAssetReader()                                                  = default;

  /**
   * @brief Read the asset of @p key with verified pixels.
   *
   * @throws std::runtime_error when the asset is missing or corrupt, or when no store is
   *         available. A reader never returns a substitute asset.
   */
  [[nodiscard]] virtual auto Read(const MaskAssetKey& key) const -> MaskAsset = 0;
};

/**
 * @brief Shared reader used when a renderer receives no reader (tools and tests).
 *
 * Each read throws "Mask asset <key> cannot be read: this renderer has no project mask asset
 * store".
 */
[[nodiscard]] auto DefaultMaskAssetReader() -> std::shared_ptr<const MaskAssetReader>;

}  // namespace alcedo
