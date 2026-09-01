//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <span>

#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/**
 * @brief Persistent R8 mask repository with a byte-budgeted host LRU.
 *
 * Contains no GPU types. @ref Put derives an immutable content-addressed key and
 * publishes a complete file without replacing a different payload. Instances are
 * not thread-safe. Concurrent @ref Put of equal content from separate stores that
 * share a root is safe. Host-cache eviction never deletes disk files. The
 * configured root is created on first publish.
 */
class MaskStore {
 public:
  explicit MaskStore(std::filesystem::path root, std::size_t host_cache_budget_bytes = 0);

  [[nodiscard]] auto Root() const -> const std::filesystem::path& { return root_; }
  void               SetHostCacheBudget(std::size_t bytes);
  [[nodiscard]] auto HostCacheBytes() const -> std::size_t { return cache_bytes_; }
  [[nodiscard]] auto HostCacheEntryCount() const -> std::size_t { return cache_.size(); }

  /**
   * @brief Persist tightly packed R8 pixels under a content-addressed key.
   *
   * @param descriptor Raster extent and reference bounds. Axes must be in [1, 4096].
   * @param pixels Row-major R8 samples owned by the caller for this call.
   * @return Immutable key for these canonical bytes. Equal inputs return the same key.
   * @throws std::invalid_argument when pixels or descriptor fail validation.
   * @throws std::runtime_error when an existing file is corrupt, collides, or
   *         publication fails. Existing published files are left unchanged.
   *         Only the incomplete temporary file is removed.
   */
  [[nodiscard]] auto Put(const MaskAssetDescriptor& descriptor,
                         std::span<const std::uint8_t> pixels) -> MaskAssetKey;

  /** @brief Load and validate one asset. Returns a shared immutable host-cache value. */
  [[nodiscard]] auto Load(const MaskAssetKey& key) -> std::shared_ptr<const MaskAsset>;

  [[nodiscard]] auto PathFor(const MaskAssetKey& key) const -> std::filesystem::path;

 private:
  struct CacheEntry {
    std::shared_ptr<const MaskAsset>  asset;
    std::list<MaskAssetKey>::iterator lru;
  };

  auto                               ReadFromDisk(const MaskAssetKey& key)
      -> std::shared_ptr<MaskAsset>;
  void                               StoreInCache(std::shared_ptr<const MaskAsset> asset);
  void                               EvictHostCache();

  std::filesystem::path              root_;
  std::size_t                        host_cache_budget_bytes_ = 0;
  std::size_t                        cache_bytes_             = 0;
  std::map<MaskAssetKey, CacheEntry> cache_;
  std::list<MaskAssetKey>            lru_;
};

/**
 * @brief Test hook invoked after the temporary file is closed and before publish.
 *
 * @param hook Receives the temporary path and intended destination. Pass nullptr
 *             to clear. Not used by product rendering.
 */
void SetMaskStorePublishHookForTesting(
    std::function<void(const std::filesystem::path& temporary,
                       const std::filesystem::path& destination)>
        hook);

}  // namespace alcedo
