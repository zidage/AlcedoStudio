//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

template <class Backend>
class MaskTextureCache;

/** @brief Move-only pin preventing eviction of a keyed workspace mask texture. */
template <class Backend>
class MaskTextureLease {
 public:
  MaskTextureLease() = default;
  MaskTextureLease(MaskTextureCache<Backend>* cache, MaskAssetKey key)
      : cache_(cache), key_(std::move(key)) {}
  MaskTextureLease(const MaskTextureLease&)                    = delete;
  auto operator=(const MaskTextureLease&) -> MaskTextureLease& = delete;
  MaskTextureLease(MaskTextureLease&& other) noexcept
      : cache_(other.cache_), key_(std::move(other.key_)) {
    other.cache_ = nullptr;
  }
  auto operator=(MaskTextureLease&& other) noexcept -> MaskTextureLease& {
    if (this != &other) {
      Release();
      cache_       = other.cache_;
      key_         = std::move(other.key_);
      other.cache_ = nullptr;
    }
    return *this;
  }
  ~MaskTextureLease() { Release(); }

  [[nodiscard]] auto Texture() -> typename Backend::Texture2D&;
  [[nodiscard]] auto Texture() const -> const typename Backend::Texture2D&;
  [[nodiscard]] auto Texture(std::size_t mip_level) -> typename Backend::Texture2D&;
  [[nodiscard]] auto MipLevelCount() const -> std::size_t;
  [[nodiscard]] auto Key() const -> const MaskAssetKey& { return key_; }
  [[nodiscard]] auto Empty() const -> bool { return cache_ == nullptr; }
  void               Release();

 private:
  MaskTextureCache<Backend>* cache_ = nullptr;
  MaskAssetKey               key_;
};

/** @brief Workspace-only byte-budget LRU keyed by persistent MaskAssetKey. */
template <class Backend>
class MaskTextureCache {
 public:
  explicit MaskTextureCache(Backend& backend) : backend_(&backend) {}
  MaskTextureCache(const MaskTextureCache&)                    = delete;
  auto operator=(const MaskTextureCache&) -> MaskTextureCache& = delete;

  void SetByteBudget(std::size_t bytes) {
    budget_bytes_ = bytes;
    EvictUntil(0);
  }
  [[nodiscard]] auto UsedBytes() const -> std::size_t { return used_bytes_; }
  [[nodiscard]] auto EntryCount() const -> std::size_t { return entries_.size(); }

  void               BeginFrame() {
    for (auto& [key, entry] : entries_) entry.used_this_frame = false;
  }

  [[nodiscard]] auto Acquire(const MaskAssetKey& key, Extent2D extent)
      -> MaskTextureLease<Backend> {
    if (key.Empty() || extent.Empty()) throw std::invalid_argument("Invalid mask texture request");
    if (auto found = entries_.find(key); found != entries_.end()) {
      if (found->second.extent != extent) {
        if (found->second.lease_count != 0 ||
            backend_->IsResourceBusy(found->second.submitted_on)) {
          throw std::runtime_error("Cannot replace active mask texture with a new extent");
        }
        used_bytes_ -= found->second.bytes;
        entries_.erase(found);
      } else {
        return TakeLease(key, found->second);
      }
    }
    EvictUntil(MipChainBytes(extent));
    Entry entry;
    entry.extent      = extent;
    auto level_extent = extent;
    while (true) {
      entry.mip_levels.push_back(
          backend_->CreateTexture2D(level_extent.width, level_extent.height, TextureFormat::R8));
      entry.bytes += entry.mip_levels.back().Bytes();
      if (level_extent.width == 1 && level_extent.height == 1) break;
      level_extent.width  = std::max<std::uint32_t>(level_extent.width / 2, 1);
      level_extent.height = std::max<std::uint32_t>(level_extent.height / 2, 1);
    }
    used_bytes_ += entry.bytes;
    auto it = entries_.emplace(key, std::move(entry)).first;
    return TakeLease(it->first, it->second);
  }

  [[nodiscard]] auto Contains(const MaskAssetKey& key) const -> bool {
    return entries_.contains(key);
  }

  void MarkSubmitted(std::uint64_t submission_id) {
    for (auto& [key, entry] : entries_) {
      if (entry.used_this_frame || entry.lease_count != 0) entry.submitted_on = submission_id;
    }
  }

  void EvictUntil(std::size_t needed_bytes) {
    if (budget_bytes_ == 0) return;
    while (used_bytes_ + needed_bytes > budget_bytes_) {
      auto victim = entries_.end();
      auto oldest = (std::numeric_limits<std::uint64_t>::max)();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const auto& entry = it->second;
        if (entry.lease_count != 0 || entry.used_this_frame ||
            backend_->IsResourceBusy(entry.submitted_on))
          continue;
        if (entry.lru_tick < oldest) {
          oldest = entry.lru_tick;
          victim = it;
        }
      }
      if (victim == entries_.end()) break;
      used_bytes_ -= victim->second.bytes;
      entries_.erase(victim);
    }
  }

  [[nodiscard]] auto TextureAt(const MaskAssetKey& key) -> typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.front();
  }
  [[nodiscard]] auto TextureAt(const MaskAssetKey& key) const -> const
      typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.front();
  }
  [[nodiscard]] auto TextureAt(const MaskAssetKey& key, std::size_t level) ->
      typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.at(level);
  }
  [[nodiscard]] auto MipLevelCount(const MaskAssetKey& key) const -> std::size_t {
    return entries_.at(key).mip_levels.size();
  }
  void Release(const MaskAssetKey& key) {
    if (auto found = entries_.find(key); found != entries_.end() && found->second.lease_count != 0)
      --found->second.lease_count;
  }

 private:
  friend class MaskTextureLease<Backend>;
  struct Entry {
    std::vector<typename Backend::Texture2D> mip_levels;
    Extent2D                                 extent{};
    std::size_t                              bytes           = 0;
    std::uint32_t                            lease_count     = 0;
    std::uint64_t                            submitted_on    = 0;
    std::uint64_t                            lru_tick        = 0;
    bool                                     used_this_frame = false;
  };
  static auto MipChainBytes(Extent2D extent) -> std::size_t {
    std::size_t bytes = 0;
    while (true) {
      bytes += static_cast<std::size_t>(extent.width) * extent.height;
      if (extent.width == 1 && extent.height == 1) return bytes;
      extent.width  = std::max<std::uint32_t>(extent.width / 2, 1);
      extent.height = std::max<std::uint32_t>(extent.height / 2, 1);
    }
  }
  auto TakeLease(const MaskAssetKey& key, Entry& entry) -> MaskTextureLease<Backend> {
    ++entry.lease_count;
    entry.used_this_frame = true;
    entry.lru_tick        = ++lru_clock_;
    return MaskTextureLease<Backend>{this, key};
  }
  Backend*                      backend_ = nullptr;
  std::map<MaskAssetKey, Entry> entries_;
  std::size_t                   budget_bytes_ = 0;
  std::size_t                   used_bytes_   = 0;
  std::uint64_t                 lru_clock_    = 0;
};

template <class Backend>
auto MaskTextureLease<Backend>::Texture() -> typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("MaskTextureLease is empty");
  return cache_->TextureAt(key_);
}
template <class Backend>
auto MaskTextureLease<Backend>::Texture() const -> const typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("MaskTextureLease is empty");
  return cache_->TextureAt(key_);
}
template <class Backend>
auto MaskTextureLease<Backend>::Texture(std::size_t mip_level) -> typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("MaskTextureLease is empty");
  return cache_->TextureAt(key_, mip_level);
}
template <class Backend>
auto MaskTextureLease<Backend>::MipLevelCount() const -> std::size_t {
  if (cache_ == nullptr) return 0;
  return cache_->MipLevelCount(key_);
}
template <class Backend>
void MaskTextureLease<Backend>::Release() {
  if (cache_ != nullptr) {
    cache_->Release(key_);
    cache_ = nullptr;
  }
}

}  // namespace alcedo
