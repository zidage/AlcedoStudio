//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

template <class Backend>
class TexturePool;

/**
 * @brief RAII pin on a TexturePool entry. The entry stays alive until released.
 *
 * The pool and its backend must outlive this object. Move-only.
 */
template <class Backend>
class ResourceLease {
 public:
  ResourceLease() = default;
  ResourceLease(TexturePool<Backend>* pool, std::uint64_t handle)
      : pool_(pool), handle_(handle) {}

  ResourceLease(const ResourceLease&)            = delete;
  auto operator=(const ResourceLease&) -> ResourceLease& = delete;

  ResourceLease(ResourceLease&& other) noexcept : pool_(other.pool_), handle_(other.handle_) {
    other.pool_   = nullptr;
    other.handle_ = 0;
  }

  auto operator=(ResourceLease&& other) noexcept -> ResourceLease& {
    if (this != &other) {
      Release();
      pool_         = other.pool_;
      handle_       = other.handle_;
      other.pool_   = nullptr;
      other.handle_ = 0;
    }
    return *this;
  }

  ~ResourceLease() { Release(); }

  void Release();

  [[nodiscard]] auto Empty() const -> bool { return pool_ == nullptr; }
  [[nodiscard]] auto Handle() const -> std::uint64_t { return handle_; }
  [[nodiscard]] auto Texture() -> typename Backend::Texture2D&;
  [[nodiscard]] auto Texture() const -> const typename Backend::Texture2D&;

 private:
  TexturePool<Backend>* pool_   = nullptr;
  std::uint64_t         handle_ = 0;
};

struct TextureRequest {
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
  TextureFormat format = TextureFormat::R8;
};

/**
 * @brief Reusable GPU textures. Matching free entries are recycled; otherwise allocate.
 *
 * Idle textures not used by this frame are reclaimed on allocation misses and at
 * frame end. Idle extents that no live lease still uses are reclaimed after GPU
 * last-use even when used_this_frame is still set from the last acquire.
 * Current-size scratch stays reusable while a result of that size remains leased.
 * Leased texture references remain stable when the pool grows. Reuse within a
 * frame requires one ordered GPU queue.
 * Not thread-safe. @p Backend must provide Texture2D, CreateTexture2D, and
 * IsResourceBusy(submitted_on_submission_id).
 */
template <class Backend>
class TexturePool {
 public:
  explicit TexturePool(Backend& backend) : backend_(&backend) {}

  TexturePool(const TexturePool&)            = delete;
  auto operator=(const TexturePool&) -> TexturePool& = delete;

  [[nodiscard]] auto UsedBytes() const -> std::size_t { return used_bytes_; }

  void BeginFrame() {
    for (auto& entry : entries_) {
      if (entry.alive) {
        entry.used_this_frame = false;
      }
    }
  }

  /**
   * @brief Reuse a matching free texture or allocate one. Increments the lease count.
   */
  [[nodiscard]] auto Acquire(const TextureRequest& request) -> ResourceLease<Backend> {
    if (request.width == 0 || request.height == 0) {
      throw std::runtime_error("TexturePool::Acquire: invalid size");
    }
    if (auto* reusable = FindReusable(request)) {
      return TakeLease(*reusable);
    }
    ReleaseUnused();
    const auto bytes = TextureBytes(request);
    auto texture = backend_->CreateTexture2D(request.width, request.height, request.format);
    Entry entry;
    entry.texture         = std::move(texture);
    entry.request         = request;
    entry.bytes           = bytes;
    entry.handle          = next_handle_++;
    entry.alive           = true;
    for (auto& vacant : entries_) {
      if (!vacant.alive) {
        vacant = std::move(entry);
        used_bytes_ += bytes;
        return TakeLease(vacant);
      }
    }
    entries_.push_back(std::move(entry));
    used_bytes_ += bytes;
    return TakeLease(entries_.back());
  }

  /**
   * @brief Extra lease on an already-leased texture. Does not allocate.
   *
   * Identity geometry uses this so `geometry.scene_source` can share
   * `develop.sensor_linear` without a second device copy.
   */
  [[nodiscard]] auto DuplicateLease(std::uint64_t handle) -> ResourceLease<Backend> {
    auto* entry = Find(handle);
    if (entry == nullptr || entry->lease_count == 0) {
      throw std::runtime_error("TexturePool::DuplicateLease: handle is not leased");
    }
    return TakeLease(*entry);
  }

  void MarkSubmitted(std::uint64_t submission_id) {
    for (auto& entry : entries_) {
      if (entry.alive && (entry.used_this_frame || entry.lease_count > 0)) {
        entry.submitted_on = submission_id;
      }
    }
  }

  [[nodiscard]] auto Contains(std::uint64_t handle) const -> bool {
    return Find(handle) != nullptr;
  }

  /**
   * @brief True when an unleased, matching texture can be reused without allocating.
   */
  [[nodiscard]] auto HasReusable(const TextureRequest& request) const -> bool {
    return FindReusable(request) != nullptr;
  }

  [[nodiscard]] auto EntryCount() const -> std::size_t {
    std::size_t count = 0;
    for (const auto& entry : entries_) {
      if (entry.alive) {
        ++count;
      }
    }
    return count;
  }

  /// Bytes pinned by image results, active scratch, or external presentation leases.
  [[nodiscard]] auto LeasedBytes() const -> std::size_t {
    std::size_t bytes = 0;
    for (const auto& entry : entries_) {
      if (entry.alive && entry.lease_count > 0) {
        bytes += entry.bytes;
      }
    }
    return bytes;
  }

  void ReleaseLease(std::uint64_t handle) {
    auto* entry = Find(handle);
    if (entry == nullptr || entry->lease_count == 0) {
      return;
    }
    --entry->lease_count;
  }

  auto TextureAt(std::uint64_t handle) -> typename Backend::Texture2D& {
    auto* entry = Find(handle);
    if (entry == nullptr) {
      throw std::runtime_error("TexturePool: invalid lease handle");
    }
    return entry->texture;
  }

  auto TextureAt(std::uint64_t handle) const -> const typename Backend::Texture2D& {
    const auto* entry = Find(handle);
    if (entry == nullptr) {
      throw std::runtime_error("TexturePool: invalid lease handle");
    }
    return entry->texture;
  }

  /**
   * @brief Destroy every unleased, idle texture. Does not wait; caller must WaitIdle first.
   *
   * GraphImageCache::Clear drops leases. This call then frees the device memory.
   */
  void ReleaseUnleased() {
    for (auto& entry : entries_) {
      if (!entry.alive || entry.lease_count > 0) {
        continue;
      }
      if (backend_->IsResourceBusy(entry.submitted_on)) {
        continue;
      }
      Destroy(entry);
    }
  }

  /**
   * @brief Destroy idle, unleased entries that this frame has not used.
   *
   * Called before allocating a new extent and after encoding a frame. Scratch
   * already referenced by recorded work remains alive, even before submission.
   * Outstanding GPU submissions and external leases also prevent destruction.
   */
  void ReleaseUnused() {
    for (auto& entry : entries_) {
      if (!entry.alive || entry.lease_count > 0 || entry.used_this_frame ||
          backend_->IsResourceBusy(entry.submitted_on)) {
        continue;
      }
      Destroy(entry);
    }
  }

  /**
   * @brief Destroy idle textures whose size is not held by any live lease.
   *
   * Crop and viewport extent changes leave previous-size scratch unleased after
   * the published result is dropped. Matching current-size scratch stays because
   * a live lease still names that extent. The this-frame use flag does not keep an
   * obsolete size: the last acquire may still have that flag when Interactive
   * identity-drops a result in the same session. Caller must WaitIdle first so
   * GPU last-use of the previous submission has completed.
   */
  void ReleaseUnleasedUnusedSizes() {
    std::set<ExtentKey> leased_extents;
    for (const auto& entry : entries_) {
      if (entry.alive && entry.lease_count > 0) {
        leased_extents.insert(MakeExtentKey(entry.request));
      }
    }
    for (auto& entry : entries_) {
      if (!entry.alive || entry.lease_count > 0 || backend_->IsResourceBusy(entry.submitted_on)) {
        continue;
      }
      if (leased_extents.contains(MakeExtentKey(entry.request))) {
        continue;
      }
      Destroy(entry);
    }
  }

  /** @brief Print texture pool totals. Per-texture lines require ALCEDO_GPU_POOL_TRACE. */
  void DumpToStderr(const char* reason) const {
    std::fprintf(stderr, "[GPU_POOL] textures %s entries=%zu used=%.1f MiB\n",
                 reason == nullptr ? "" : reason, EntryCount(), GpuPoolMiB(used_bytes_));
    if (!GpuPoolTraceVerbose()) {
      return;
    }
    for (const auto& entry : entries_) {
      if (!entry.alive) {
        continue;
      }
      std::fprintf(stderr,
                   "[GPU_POOL]   tex handle=%llu %ux%u %s %.1f MiB leases=%u busy_sub=%llu "
                   "frame=%d\n",
                   static_cast<unsigned long long>(entry.handle), entry.request.width,
                   entry.request.height, TextureFormatName(entry.request.format),
                   GpuPoolMiB(entry.bytes), entry.lease_count,
                   static_cast<unsigned long long>(entry.submitted_on),
                   entry.used_this_frame ? 1 : 0);
    }
  }

 private:
  friend class ResourceLease<Backend>;

  struct Entry {
    typename Backend::Texture2D texture{};
    TextureRequest              request{};
    std::size_t                 bytes           = 0;
    std::uint64_t               handle          = 0;
    std::uint32_t               lease_count     = 0;
    std::uint64_t               submitted_on    = 0;
    bool                        used_this_frame = false;
    bool                        alive           = false;
  };

  struct ExtentKey {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::R8;

    friend auto operator<(const ExtentKey& lhs, const ExtentKey& rhs) -> bool {
      return std::tie(lhs.width, lhs.height, lhs.format) <
             std::tie(rhs.width, rhs.height, rhs.format);
    }
  };

  static auto MakeExtentKey(const TextureRequest& request) -> ExtentKey {
    return ExtentKey{request.width, request.height, request.format};
  }

  static auto TextureBytes(const TextureRequest& request) -> std::size_t {
    return static_cast<std::size_t>(request.width) * request.height *
           TextureFormatBytesPerPixel(request.format);
  }

  void Destroy(Entry& entry) {
    used_bytes_ -= entry.bytes;
    entry.texture = {};
    entry.alive   = false;
  }

  auto Find(std::uint64_t handle) -> Entry* {
    for (auto& entry : entries_) {
      if (entry.alive && entry.handle == handle) {
        return &entry;
      }
    }
    return nullptr;
  }

  auto Find(std::uint64_t handle) const -> const Entry* {
    for (const auto& entry : entries_) {
      if (entry.alive && entry.handle == handle) {
        return &entry;
      }
    }
    return nullptr;
  }

  auto FindReusable(const TextureRequest& request) -> Entry* {
    return const_cast<Entry*>(static_cast<const TexturePool*>(this)->FindReusable(request));
  }

  auto FindReusable(const TextureRequest& request) const -> const Entry* {
    for (const auto& entry : entries_) {
      if (!entry.alive || entry.lease_count > 0 || backend_->IsResourceBusy(entry.submitted_on)) {
        continue;
      }
      if (entry.request.width != request.width || entry.request.height != request.height ||
          entry.request.format != request.format) {
        continue;
      }
      return &entry;
    }
    return nullptr;
  }

  auto TakeLease(Entry& entry) -> ResourceLease<Backend> {
    ++entry.lease_count;
    entry.used_this_frame = true;
    return ResourceLease<Backend>{this, entry.handle};
  }

  Backend*           backend_      = nullptr;
  std::deque<Entry> entries_;
  std::size_t        used_bytes_   = 0;
  std::uint64_t      next_handle_  = 1;
};

template <class Backend>
void ResourceLease<Backend>::Release() {
  if (pool_ != nullptr) {
    pool_->ReleaseLease(handle_);
    pool_   = nullptr;
    handle_ = 0;
  }
}

template <class Backend>
auto ResourceLease<Backend>::Texture() -> typename Backend::Texture2D& {
  if (pool_ == nullptr) {
    throw std::runtime_error("ResourceLease: empty");
  }
  return pool_->TextureAt(handle_);
}

template <class Backend>
auto ResourceLease<Backend>::Texture() const -> const typename Backend::Texture2D& {
  if (pool_ == nullptr) {
    throw std::runtime_error("ResourceLease: empty");
  }
  return pool_->TextureAt(handle_);
}

}  // namespace alcedo
