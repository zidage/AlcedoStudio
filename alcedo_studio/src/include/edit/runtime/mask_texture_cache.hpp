//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

template <class Backend, class Key>
class RasterTextureCache;

/** @brief Move-only pin preventing eviction of a keyed workspace R8 texture. */
template <class Backend, class Key>
class RasterTextureLease {
 public:
  RasterTextureLease() = default;
  RasterTextureLease(RasterTextureCache<Backend, Key>* cache, Key key)
      : cache_(cache), key_(std::move(key)) {}
  RasterTextureLease(const RasterTextureLease&)                    = delete;
  auto operator=(const RasterTextureLease&) -> RasterTextureLease& = delete;
  RasterTextureLease(RasterTextureLease&& other) noexcept
      : cache_(other.cache_), key_(std::move(other.key_)) {
    other.cache_ = nullptr;
  }
  auto operator=(RasterTextureLease&& other) noexcept -> RasterTextureLease& {
    if (this != &other) {
      Release();
      cache_       = other.cache_;
      key_         = std::move(other.key_);
      other.cache_ = nullptr;
    }
    return *this;
  }
  ~RasterTextureLease() { Release(); }

  [[nodiscard]] auto Texture() -> typename Backend::Texture2D&;
  [[nodiscard]] auto Texture() const -> const typename Backend::Texture2D&;
  [[nodiscard]] auto Texture(std::size_t mip_level) -> typename Backend::Texture2D&;
  [[nodiscard]] auto MipLevelCount() const -> std::size_t;
  [[nodiscard]] auto CacheKey() const -> const Key& { return key_; }
  [[nodiscard]] auto Empty() const -> bool { return cache_ == nullptr; }
  void               Release();

 private:
  RasterTextureCache<Backend, Key>* cache_ = nullptr;
  Key                               key_{};
};

/** @brief Workspace-only byte-budget LRU for persistent or active R8 mask textures. */
template <class Backend, class Key>
class RasterTextureCache {
 public:
  explicit RasterTextureCache(Backend& backend) : backend_(&backend) {}
  RasterTextureCache(const RasterTextureCache&)                    = delete;
  auto operator=(const RasterTextureCache&) -> RasterTextureCache& = delete;

  void SetByteBudget(std::size_t bytes) {
    budget_bytes_ = bytes;
    EvictUntil(0);
  }
  [[nodiscard]] auto UsedBytes() const -> std::size_t { return used_bytes_; }
  [[nodiscard]] auto EntryCount() const -> std::size_t { return entries_.size(); }

  /**
   * @brief Drop every mask texture. Caller must not hold RasterTextureLease objects.
   */
  void Clear() {
    entries_.clear();
    used_bytes_ = 0;
  }

  void               BeginFrame() {
    for (auto& [key, entry] : entries_) entry.used_this_frame = false;
  }

  [[nodiscard]] auto Acquire(const Key& key, Extent2D extent) -> RasterTextureLease<Backend, Key> {
    if (extent.Empty()) throw std::invalid_argument("Invalid mask texture request");
    if constexpr (requires { key.Empty(); }) {
      if (key.Empty()) throw std::invalid_argument("Invalid mask texture request");
    }
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
    if (ShouldTraceGpuPoolAlloc(it->second.bytes) && GpuPoolTraceVerbose()) {
      DumpToStderr("mask-alloc");
    }
    return TakeLease(it->first, it->second);
  }

  void DumpToStderr(const char* reason) const {
    std::fprintf(stderr, "[GPU_POOL] masks %s entries=%zu used=%.1f MiB\n",
                 reason == nullptr ? "" : reason, entries_.size(), GpuPoolMiB(used_bytes_));
    if (!GpuPoolTraceVerbose()) {
      return;
    }
    for (const auto& [key, entry] : entries_) {
      const auto text = KeyDebugText(key);
      std::fprintf(stderr, "[GPU_POOL]   mask %s %ux%u mips=%zu %.1f MiB leases=%u\n", text.c_str(),
                   entry.extent.width, entry.extent.height, entry.mip_levels.size(),
                   GpuPoolMiB(entry.bytes), entry.lease_count);
    }
  }

  [[nodiscard]] auto Contains(const Key& key) const -> bool { return entries_.contains(key); }

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

  template <class Pred>
  void EraseIdleIf(Pred pred) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!pred(it->first)) {
        ++it;
        continue;
      }
      const auto& entry = it->second;
      if (entry.lease_count != 0 || entry.used_this_frame ||
          backend_->IsResourceBusy(entry.submitted_on)) {
        ++it;
        continue;
      }
      used_bytes_ -= entry.bytes;
      it = entries_.erase(it);
    }
  }

  [[nodiscard]] auto TextureAt(const Key& key) -> typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.front();
  }
  [[nodiscard]] auto TextureAt(const Key& key) const -> const typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.front();
  }
  [[nodiscard]] auto TextureAt(const Key& key, std::size_t level) -> typename Backend::Texture2D& {
    return entries_.at(key).mip_levels.at(level);
  }
  [[nodiscard]] auto MipLevelCount(const Key& key) const -> std::size_t {
    return entries_.at(key).mip_levels.size();
  }
  [[nodiscard]] auto UploadedRevision(const Key& key) const -> std::uint64_t {
    return entries_.at(key).uploaded_revision;
  }
  [[nodiscard]] auto PixelsUploaded(const Key& key) const -> bool {
    return entries_.at(key).pixels_uploaded;
  }
  void SetUploadedPixels(const Key& key, std::uint64_t revision) {
    auto& entry            = entries_.at(key);
    entry.uploaded_revision = revision;
    entry.pixels_uploaded   = true;
  }
  void Release(const Key& key) {
    if (auto found = entries_.find(key); found != entries_.end() && found->second.lease_count != 0)
      --found->second.lease_count;
  }

 private:
  friend class RasterTextureLease<Backend, Key>;
  struct Entry {
    std::vector<typename Backend::Texture2D> mip_levels;
    Extent2D                                 extent{};
    std::size_t                              bytes             = 0;
    std::uint32_t                            lease_count       = 0;
    std::uint64_t                            submitted_on      = 0;
    std::uint64_t                            lru_tick          = 0;
    std::uint64_t                            uploaded_revision = 0;
    bool                                     used_this_frame   = false;
    bool                                     pixels_uploaded   = false;
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
  static auto KeyDebugText(const Key& key) -> std::string {
    if constexpr (requires { key.Value(); }) {
      return std::string{key.Value()};
    } else if constexpr (requires { key.DebugText(); }) {
      return key.DebugText();
    } else {
      return {};
    }
  }
  auto TakeLease(const Key& key, Entry& entry) -> RasterTextureLease<Backend, Key> {
    ++entry.lease_count;
    entry.used_this_frame = true;
    entry.lru_tick        = ++lru_clock_;
    return RasterTextureLease<Backend, Key>{this, key};
  }
  Backend*               backend_ = nullptr;
  std::map<Key, Entry>   entries_;
  std::size_t            budget_bytes_ = 0;
  std::size_t            used_bytes_   = 0;
  std::uint64_t          lru_clock_    = 0;
};

template <class Backend>
using MaskTextureCache = RasterTextureCache<Backend, MaskAssetKey>;
template <class Backend>
using MaskTextureLease = RasterTextureLease<Backend, MaskAssetKey>;
template <class Backend>
using ActiveRasterTextureCache = RasterTextureCache<Backend, ActiveRasterTextureKey>;
template <class Backend>
using ActiveRasterTextureLease = RasterTextureLease<Backend, ActiveRasterTextureKey>;

template <class Backend, class Key>
auto RasterTextureLease<Backend, Key>::Texture() -> typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("RasterTextureLease is empty");
  return cache_->TextureAt(key_);
}
template <class Backend, class Key>
auto RasterTextureLease<Backend, Key>::Texture() const -> const typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("RasterTextureLease is empty");
  return cache_->TextureAt(key_);
}
template <class Backend, class Key>
auto RasterTextureLease<Backend, Key>::Texture(std::size_t mip_level) ->
    typename Backend::Texture2D& {
  if (cache_ == nullptr) throw std::runtime_error("RasterTextureLease is empty");
  return cache_->TextureAt(key_, mip_level);
}
template <class Backend, class Key>
auto RasterTextureLease<Backend, Key>::MipLevelCount() const -> std::size_t {
  if (cache_ == nullptr) return 0;
  return cache_->MipLevelCount(key_);
}
template <class Backend, class Key>
void RasterTextureLease<Backend, Key>::Release() {
  if (cache_ != nullptr) {
    cache_->Release(key_);
    cache_ = nullptr;
  }
}

}  // namespace alcedo
