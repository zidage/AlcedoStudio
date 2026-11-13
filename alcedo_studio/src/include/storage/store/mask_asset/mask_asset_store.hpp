//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstddef>
#include <set>
#include <vector>

#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/// Codec text of the `MaskAsset.data` column: zlib `compress2` of the tightly packed R8 pixels.
inline constexpr const char* kMaskAssetZlibCodec = "zlib";

/**
 * @brief Rows of the `MaskAsset` table of the project database: one content-addressed R8 raster
 *        per row.
 *
 * The caller owns @p conn and holds the database lock for each call (see Database). A put of an
 * existing key writes nothing, so a row never changes after its first write. A read returns only
 * pixels whose byte count and recomputed key match the row.
 *
 * Errors are `std::runtime_error`; a failed call leaves the table unchanged.
 */
class MaskAssetStore {
 public:
  /// @param conn Open connection; it must outlive the store.
  explicit MaskAssetStore(duckdb_connection conn) : conn_(conn) {}

  /**
   * @brief Insert @p asset when its key is not stored.
   *
   * Validates the raster, recomputes MakeMaskAssetKey and requires it to equal `asset.key`, and
   * stores the pixels compressed with zlib. Runs as one statement (its own transaction unless the
   * caller has one open).
   *
   * @return true when a row was written; false when the key was already stored.
   * @throws std::runtime_error when the asset is invalid, the key differs, compression fails, or
   *         the SQL fails.
   */
  auto Put(const MaskAsset& asset) -> bool;

  /**
   * @brief Read and verify the asset of @p key.
   * @throws std::runtime_error "Mask asset <key> is missing from the project" when no row has the
   *         key, or "Mask asset <key> is corrupt: <reason>" when the format, codec, byte count,
   *         dimensions, or recomputed key do not match.
   */
  auto Read(const MaskAssetKey& key) -> MaskAsset;

  /// Every stored key in ascending order.
  auto ListKeys() -> std::vector<MaskAssetKey>;

  /**
   * @brief Delete every row whose key is not in @p keep, in one transaction.
   * @return Number of deleted rows.
   * @throws std::runtime_error after a rollback when a statement fails; then no row is deleted.
   */
  auto DeleteKeysNotIn(const std::set<MaskAssetKey>& keep) -> std::size_t;

 private:
  duckdb_connection conn_;
};

}  // namespace alcedo
