//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace alcedo {

/**
 * @brief Grow-only bump allocator over one device slab supplied by @p Backend.
 *
 * Backend must provide:
 * - `class Slab` with `DevicePointer()`, `Bytes()`, move-only RAII free
 * - `CreateSlab(std::size_t bytes) -> Slab`
 *
 * Capacity grows only when no bump allocations are live (`used_bytes() == 0`).
 * `Reset()` / `Rewind` do not free the slab. Not thread-safe.
 *
 * @tparam Backend Slab factory. Must outlive this arena when passed by reference.
 */
template <class Backend>
class TransientBufferArena {
 public:
  static constexpr std::size_t kDefaultAlignment = 256;

  TransientBufferArena() : owned_(std::in_place), backend_(&*owned_) {}

  explicit TransientBufferArena(std::size_t initial_capacity_bytes) : TransientBufferArena() {
    Reserve(initial_capacity_bytes);
  }

  /**
   * @brief Borrow an existing backend so allocation counters stay on one device.
   * @pre backend outlives this arena.
   */
  explicit TransientBufferArena(Backend& backend) : backend_(&backend) {}

  TransientBufferArena(Backend& backend, std::size_t initial_capacity_bytes)
      : backend_(&backend) {
    Reserve(initial_capacity_bytes);
  }

  TransientBufferArena(const TransientBufferArena&)            = delete;
  auto operator=(const TransientBufferArena&) -> TransientBufferArena& = delete;

  TransientBufferArena(TransientBufferArena&& other) noexcept { MoveFrom(other); }

  auto operator=(TransientBufferArena&& other) noexcept -> TransientBufferArena& {
    if (this != &other) {
      ResetSlab();
      MoveFrom(other);
    }
    return *this;
  }

  ~TransientBufferArena() { ResetSlab(); }

  /**
   * @brief Ensure slab capacity is at least @p bytes. Grows only when unused.
   * @throws std::runtime_error if bump allocations are live.
   */
  void Reserve(std::size_t bytes) {
    if (bytes <= capacity_) {
      return;
    }
    if (offset_ != 0) {
      throw std::runtime_error(
          "TransientBufferArena::Reserve: cannot grow while allocations are live; "
          "Reset() first or Reserve peak size before Allocate");
    }
    auto new_slab = backend_->CreateSlab(bytes);
    slab_         = std::move(new_slab);
    capacity_     = bytes;
  }

  /**
   * @brief Bump-allocate @p bytes. Zero returns nullptr.
   * @throws std::runtime_error on bad alignment, or if capacity is short while live.
   */
  [[nodiscard]] auto Allocate(std::size_t bytes, std::size_t alignment = kDefaultAlignment)
      -> void* {
    if (bytes == 0) {
      return nullptr;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
      throw std::runtime_error("TransientBufferArena::Allocate: alignment must be a power of two");
    }

    const std::size_t aligned_offset = AlignUp(offset_, alignment);
    const std::size_t end            = aligned_offset + bytes;

    if (end > capacity_) {
      if (offset_ != 0) {
        throw std::runtime_error(
            "TransientBufferArena::Allocate: insufficient capacity with live allocations; "
            "call Reserve() with the peak workspace size before forward");
      }
      std::size_t new_cap = capacity_ == 0 ? end : capacity_;
      while (new_cap < end) {
        const std::size_t doubled = new_cap > (std::size_t{1} << 62) ? end : new_cap * 2;
        new_cap                   = doubled < end ? end : doubled;
      }
      Reserve(new_cap);
      offset_ = bytes;
      return base();
    }

    offset_ = end;
    return static_cast<char*>(base()) + aligned_offset;
  }

  template <typename T>
  [[nodiscard]] auto Allocate(std::size_t count, std::size_t alignment = kDefaultAlignment) -> T* {
    return static_cast<T*>(Allocate(count * sizeof(T), alignment));
  }

  /// Rewind bump pointer to zero. Does not free device memory.
  void Reset() noexcept { offset_ = 0; }

  /// Free the device slab. Requires no live bump allocations (`used_bytes() == 0`).
  void ReleaseDeviceMemory() noexcept { ResetSlab(); }

  void Rewind(std::size_t mark_bytes) {
    if (mark_bytes > offset_) {
      throw std::runtime_error("TransientBufferArena::Rewind: mark is past current offset");
    }
    offset_ = mark_bytes;
  }

  void RewindUnchecked(std::size_t mark_bytes) noexcept {
    offset_ = mark_bytes < offset_ ? mark_bytes : offset_;
  }

  [[nodiscard]] auto capacity_bytes() const noexcept -> std::size_t { return capacity_; }
  [[nodiscard]] auto used_bytes() const noexcept -> std::size_t { return offset_; }
  [[nodiscard]] auto remaining_bytes() const noexcept -> std::size_t {
    return capacity_ > offset_ ? capacity_ - offset_ : 0;
  }
  [[nodiscard]] auto base() const noexcept -> void* {
    return slab_.DevicePointer();
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return offset_ == 0; }

 private:
  static auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void ResetSlab() noexcept {
    slab_     = {};
    capacity_ = 0;
    offset_   = 0;
  }

  void MoveFrom(TransientBufferArena& other) noexcept {
    owned_ = std::move(other.owned_);
    if (owned_.has_value()) {
      backend_       = &*owned_;
      other.backend_ = nullptr;
    } else {
      backend_       = other.backend_;
      other.backend_ = nullptr;
    }
    slab_           = std::move(other.slab_);
    capacity_       = other.capacity_;
    offset_         = other.offset_;
    other.capacity_ = 0;
    other.offset_   = 0;
  }

  std::optional<Backend> owned_;
  Backend*               backend_ = nullptr;
  typename Backend::Slab slab_{};
  std::size_t            capacity_ = 0;
  std::size_t            offset_   = 0;
};

}  // namespace alcedo
