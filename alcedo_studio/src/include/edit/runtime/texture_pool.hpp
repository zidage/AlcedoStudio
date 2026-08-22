//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "edit/runtime/texture_format.hpp"

namespace alcedo {

template <class Backend>
class TexturePool;

/**
 * @brief RAII pin on a TexturePool entry. Prevents LRU eviction while held.
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
 * @brief Byte-budget LRU of GPU textures. Evicts only unleased, non-busy entries.
 *
 * Not thread-safe. @p Backend must provide Texture2D, CreateTexture2D, and
 * IsResourceBusy(submitted_on_submission_id).
 */
template <class Backend>
class TexturePool {
 public:
  explicit TexturePool(Backend& backend) : backend_(&backend) {}

  TexturePool(const TexturePool&)            = delete;
  auto operator=(const TexturePool&) -> TexturePool& = delete;

  void SetByteBudget(std::size_t bytes) { budget_bytes_ = bytes; }
  [[nodiscard]] auto ByteBudget() const -> std::size_t { return budget_bytes_; }
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
    const auto bytes = TextureBytes(request);
    if (auto* reusable = FindReusable(request)) {
      return TakeLease(*reusable);
    }
    EvictUntil(bytes);
    auto texture = backend_->CreateTexture2D(request.width, request.height, request.format);
    Entry entry;
    entry.texture         = std::move(texture);
    entry.request         = request;
    entry.bytes           = bytes;
    entry.handle          = next_handle_++;
    entry.alive           = true;
    entries_.push_back(std::move(entry));
    used_bytes_ += bytes;
    return TakeLease(entries_.back());
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

  [[nodiscard]] auto EntryCount() const -> std::size_t {
    std::size_t count = 0;
    for (const auto& entry : entries_) {
      if (entry.alive) {
        ++count;
      }
    }
    return count;
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
   * @brief Drop unleased, idle textures until used + needed fits the budget, if possible.
   */
  void EvictUntil(std::size_t needed_bytes) {
    if (budget_bytes_ == 0) {
      return;
    }
    while (used_bytes_ + needed_bytes > budget_bytes_) {
      Entry* victim = nullptr;
      std::uint64_t oldest = (std::numeric_limits<std::uint64_t>::max)();
      for (auto& entry : entries_) {
        if (!entry.alive || entry.lease_count > 0) {
          continue;
        }
        if (backend_->IsResourceBusy(entry.submitted_on)) {
          continue;
        }
        if (entry.lru_tick < oldest) {
          oldest = entry.lru_tick;
          victim = &entry;
        }
      }
      if (victim == nullptr) {
        break;
      }
      used_bytes_ -= victim->bytes;
      victim->texture = {};
      victim->alive   = false;
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
    std::uint64_t               lru_tick        = 0;
    bool                        used_this_frame = false;
    bool                        alive           = false;
  };

  static auto TextureBytes(const TextureRequest& request) -> std::size_t {
    return static_cast<std::size_t>(request.width) * request.height *
           TextureFormatBytesPerPixel(request.format);
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
    for (auto& entry : entries_) {
      if (!entry.alive || entry.lease_count > 0) {
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
    entry.lru_tick        = ++lru_clock_;
    return ResourceLease<Backend>{this, entry.handle};
  }

  Backend*             backend_      = nullptr;
  std::vector<Entry>   entries_;
  std::size_t          budget_bytes_ = 0;
  std::size_t          used_bytes_   = 0;
  std::uint64_t        next_handle_  = 1;
  std::uint64_t        lru_clock_    = 0;
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
