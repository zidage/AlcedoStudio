//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "sleeve/storage.hpp"

namespace alcedo {

/**
 * @brief Reads the Mask assets of one project database for its renderers.
 *
 * Each read takes the database connection guard and its lock for the call, as
 * PipelineMgmtService::ReadMaskAsset does, so render threads can read at the same time as other
 * project operations.
 */
class ProjectMaskAssetReader final : public MaskAssetReader {
 public:
  /// @param storage Project storage. A null storage makes each read fail.
  explicit ProjectMaskAssetReader(std::shared_ptr<Storage> storage);

  /**
   * @throws std::runtime_error "Mask asset <key> is missing from the project" or "Mask asset
   *         <key> is corrupt: <reason>".
   */
  [[nodiscard]] auto Read(const MaskAssetKey& key) const -> MaskAsset override;

 private:
  std::shared_ptr<Storage> storage_;
};

}  // namespace alcedo
