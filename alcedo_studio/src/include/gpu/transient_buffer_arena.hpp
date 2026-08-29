//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

/**
 * @brief Grow-only bump allocator over device slabs supplied by @p Backend.
 *
 * Backend must provide:
 * - `class Slab` with `DevicePointer()`, `Bytes()`, move-only RAII free
 * - `CreateSlab(std::size_t bytes) -> Slab`
 *
 * Optional: `MaxSlabBytes()` caps each `CreateSlab`. Backends with a per-buffer
 * allocation limit report it here. A single `Allocate` must fit in one slab.
 *
 * `Reserve` may replace unused slabs. `Allocate` that does not fit the current
 * remainder appends another slab; live pointers stay valid. Develop scratch is
 * exclusive and discarded after SensorDevelop (`ReleaseDeviceMemory`), so extra
 * slabs in that pass are not held across Geometry / Grade / DRT.
 * `Reset()` / `Rewind` do not free slabs. Not thread-safe.
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
    const auto previous = capacity_;
    EnsureCapacity(bytes);
    if (ShouldTraceGpuPoolAlloc(bytes)) {
      std::fprintf(stderr, "[GPU_POOL] transient reserve %.1f MiB (was %.1f)\n", GpuPoolMiB(bytes),
                   GpuPoolMiB(previous));
      if constexpr (requires(const Backend& backend) { backend.QueryDeviceMemory(); }) {
        PrintGpuDeviceMemory(backend_->QueryDeviceMemory());
      }
    }
  }

  /**
   * @brief Bump-allocate @p bytes. Zero returns nullptr.
   *
   * Places into an existing slab when there is room. Otherwise appends a new slab.
   * @throws std::runtime_error on bad alignment or a request larger than one slab.
   */
  [[nodiscard]] auto Allocate(std::size_t bytes, std::size_t alignment = kDefaultAlignment)
      -> void* {
    if (bytes == 0) {
      return nullptr;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
      throw std::runtime_error("TransientBufferArena::Allocate: alignment must be a power of two");
    }
    const auto max_slab = QueryMaxSlabBytes();
    if (bytes > max_slab) {
      throw std::runtime_error(
          "TransientBufferArena::Allocate: request exceeds backend max slab bytes");
    }

    std::size_t aligned_offset = AlignUp(offset_, alignment);
    if (void* placed = PlaceFrom(aligned_offset, bytes, alignment)) {
      return placed;
    }

    if (offset_ != 0) {
      return AppendAndPlace(bytes);
    }
    const std::size_t end = aligned_offset + bytes;
    std::size_t new_cap = capacity_ == 0 ? end : capacity_;
    while (new_cap < end) {
      const std::size_t doubled = new_cap > (std::size_t{1} << 62) ? end : new_cap * 2;
      new_cap                   = doubled < end ? end : doubled;
    }
    Reserve(new_cap);
    void* ptr = TryPlace(0, bytes);
    if (ptr == nullptr) {
      return AppendAndPlace(bytes);
    }
    offset_ = bytes;
    return ptr;
  }

  template <typename T>
  [[nodiscard]] auto Allocate(std::size_t count, std::size_t alignment = kDefaultAlignment) -> T* {
    return static_cast<T*>(Allocate(count * sizeof(T), alignment));
  }

  /// Rewind bump pointer to zero. Does not free device memory.
  void Reset() noexcept { offset_ = 0; }

  /**
   * @brief Free device slabs. Caller must GPU-synchronize if kernels still read them.
   *
   * Develop scratch is released after SensorDevelop, not kept for later frames.
   */
  void ReleaseDeviceMemory() noexcept {
    if (capacity_ > 0 && ShouldTraceGpuPoolAlloc(capacity_)) {
      std::fprintf(stderr, "[GPU_POOL] transient release %.1f MiB\n", GpuPoolMiB(capacity_));
      if constexpr (requires(const Backend& backend) { backend.QueryDeviceMemory(); }) {
        PrintGpuDeviceMemory(backend_->QueryDeviceMemory());
      }
    }
    ResetSlab();
  }

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
    return slabs_.empty() ? nullptr : slabs_.front().slab.DevicePointer();
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return offset_ == 0; }

 private:
  struct SlabEntry {
    typename Backend::Slab slab{};
    std::size_t            size = 0;
  };

  static auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  auto QueryMaxSlabBytes() const -> std::size_t {
    if (backend_ == nullptr) {
      return (std::numeric_limits<std::size_t>::max)();
    }
    if constexpr (requires(const Backend& backend) { backend.MaxSlabBytes(); }) {
      const auto cap = backend_->MaxSlabBytes();
      if (cap != 0) {
        return cap;
      }
    }
    return (std::numeric_limits<std::size_t>::max)();
  }

  auto TryPlace(std::size_t aligned_offset, std::size_t bytes) -> void* {
    std::size_t origin = 0;
    for (auto& entry : slabs_) {
      if (aligned_offset >= origin && aligned_offset + bytes <= origin + entry.size) {
        return static_cast<char*>(entry.slab.DevicePointer()) + (aligned_offset - origin);
      }
      origin += entry.size;
    }
    return nullptr;
  }

  auto NextSlabOrigin(std::size_t aligned_offset) const -> std::size_t {
    std::size_t origin = 0;
    for (const auto& entry : slabs_) {
      const auto end = origin + entry.size;
      if (aligned_offset < end) {
        return end;
      }
      origin = end;
    }
    return origin;
  }

  auto PlaceFrom(std::size_t aligned_offset, std::size_t bytes, std::size_t alignment) -> void* {
    while (true) {
      if (void* placed = TryPlace(aligned_offset, bytes)) {
        offset_ = aligned_offset + bytes;
        return placed;
      }
      const auto skipped = NextSlabOrigin(aligned_offset);
      if (skipped == aligned_offset) {
        return nullptr;
      }
      const auto next = AlignUp(skipped, alignment);
      if (next <= aligned_offset) {
        return nullptr;
      }
      aligned_offset = next;
    }
  }

  auto AppendAndPlace(std::size_t bytes) -> void* {
    const auto origin = capacity_;
    AppendSlab(bytes);
    void* placed = TryPlace(origin, bytes);
    if (placed == nullptr) {
      throw std::runtime_error("TransientBufferArena::Allocate: appended slab did not fit");
    }
    offset_ = origin + bytes;
    return placed;
  }

  void AppendSlab(std::size_t bytes) {
    const auto max_slab = QueryMaxSlabBytes();
    const auto chunk    = bytes > max_slab ? max_slab : bytes;
    slabs_.push_back(SlabEntry{backend_->CreateSlab(chunk), chunk});
    capacity_ += chunk;
  }

  void EnsureCapacity(std::size_t bytes) {
    // Drop unused slabs first. Creating the replacement before release would
    // keep both working sets alive and double the VRAM occupancy.
    ResetSlab();
    std::vector<SlabEntry> replacement;
    std::size_t            total     = 0;
    const auto             max_slab  = QueryMaxSlabBytes();
    std::size_t            remaining = bytes;
    while (remaining > 0) {
      const auto chunk = remaining > max_slab ? max_slab : remaining;
      replacement.push_back(SlabEntry{backend_->CreateSlab(chunk), chunk});
      total += chunk;
      remaining -= chunk;
    }
    slabs_    = std::move(replacement);
    capacity_ = total;
  }

  void ResetSlab() noexcept {
    slabs_.clear();
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
    slabs_          = std::move(other.slabs_);
    capacity_       = other.capacity_;
    offset_         = other.offset_;
    other.capacity_ = 0;
    other.offset_   = 0;
  }

  std::optional<Backend> owned_;
  Backend*               backend_ = nullptr;
  std::vector<SlabEntry> slabs_;
  std::size_t            capacity_ = 0;
  std::size_t            offset_   = 0;
};

}  // namespace alcedo
