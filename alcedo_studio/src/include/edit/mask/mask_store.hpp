//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <list>
#include <map>
#include <memory>

#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/**
 * @brief Persistent R8 mask repository with a byte-budgeted host LRU.
 *
 * Contains no GPU types. Save writes a complete sibling file and atomically replaces the target.
 * Instances are not thread-safe. The configured root is created on first save.
 */
class MaskStore {
 public:
  explicit MaskStore(std::filesystem::path root, std::size_t host_cache_budget_bytes = 0);

  [[nodiscard]] auto Root() const -> const std::filesystem::path& { return root_; }
  void               SetHostCacheBudget(std::size_t bytes);
  [[nodiscard]] auto HostCacheBytes() const -> std::size_t { return cache_bytes_; }
  [[nodiscard]] auto HostCacheEntryCount() const -> std::size_t { return cache_.size(); }

  /** @brief Atomically persist one validated R8 asset and refresh its host-cache entry. */
  void               Save(const MaskAsset& asset);

  /** @brief Load and validate one asset. Returns a shared immutable host-cache value. */
  [[nodiscard]] auto Load(const MaskAssetKey& key) -> std::shared_ptr<const MaskAsset>;

  [[nodiscard]] auto PathFor(const MaskAssetKey& key) const -> std::filesystem::path;

 private:
  struct CacheEntry {
    std::shared_ptr<const MaskAsset>  asset;
    std::list<MaskAssetKey>::iterator lru;
  };

  void                               StoreInCache(std::shared_ptr<const MaskAsset> asset);
  void                               EvictHostCache();

  std::filesystem::path              root_;
  std::size_t                        host_cache_budget_bytes_ = 0;
  std::size_t                        cache_bytes_             = 0;
  std::map<MaskAssetKey, CacheEntry> cache_;
  std::list<MaskAssetKey>            lru_;
};

}  // namespace alcedo
