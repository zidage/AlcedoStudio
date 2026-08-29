//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

struct TransientArenaSnapshot {
  std::size_t requested_bytes    = 0;
  std::size_t padding_bytes      = 0;
  std::size_t used_bytes        = 0;
  std::size_t capacity_bytes    = 0;
  std::size_t unused_tail_bytes = 0;
  std::size_t slab_count        = 0;
  std::size_t largest_allocation = 0;
  std::size_t grow_count        = 0;
};

/**
 * @brief Independent device slabs with a local bump pointer in each slab.
 *
 * Backend must provide:
 * - `class Slab` with `DevicePointer()`, `Bytes()`, move-only RAII free
 * - `CreateSlab(std::size_t bytes) -> Slab`
 *
 * Optional: `MaxSlabBytes()` caps each `CreateSlab`. A single `Allocate` must
 * fit in one slab; allocations never span slab boundaries and never relocate
 * live device pointers. Optional `MaxTransientBytes()` caps the sum of slabs.
 *
 * `Reserve` may replace unused slabs. `Allocate` that does not fit any current
 * remainder appends another slab. Develop scratch is exclusive and discarded
 * after SensorDevelop (`ReleaseDeviceMemory`). `Reset()` rewinds every local
 * bump without freeing slabs. Not thread-safe.
 *
 * @tparam Backend Slab factory. Must outlive this arena when passed by reference.
 */
template <class Backend>
class TransientBufferArena {
 public:
  static constexpr std::size_t kDefaultAlignment = 256;
  static constexpr std::size_t kMinSlabBytes    = 16ull << 20;
  static constexpr std::size_t kSlabQuantumBytes = 64ull << 20;

  using Mark = std::vector<std::size_t>;

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
    if (bytes <= capacity_bytes() && !slabs_.empty()) {
      return;
    }
    if (HasLiveAllocations()) {
      throw std::runtime_error(
          "TransientBufferArena::Reserve: cannot grow while allocations are live; "
          "Reset() first or Reserve peak size before Allocate");
    }
    const auto previous = capacity_bytes();
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
   * @brief Bump-allocate @p bytes inside one slab. Zero returns nullptr.
   *
   * Tries each existing slab's local bump. Otherwise appends a new slab sized
   * for the request. Live pointers stay valid.
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
      throw std::runtime_error(FormatLimitError(
          "TransientBufferArena::Allocate: request exceeds backend max slab bytes", bytes));
    }

    for (auto& entry : slabs_) {
      if (void* placed = TryAllocateIn(entry, bytes, alignment)) {
        NoteAllocation(bytes);
        return placed;
      }
    }

    const auto slab_bytes = SlabBytesForRequest(bytes);
    AppendSlab(slab_bytes);
    ++grow_count_;
    if (void* placed = TryAllocateIn(slabs_.back(), bytes, alignment)) {
      NoteAllocation(bytes);
      return placed;
    }
    throw std::runtime_error(
        FormatLimitError("TransientBufferArena::Allocate: new transient slab cannot satisfy "
                         "allocation",
                         bytes));
  }

  template <typename T>
  [[nodiscard]] auto Allocate(std::size_t count, std::size_t alignment = kDefaultAlignment) -> T* {
    return static_cast<T*>(Allocate(count * sizeof(T), alignment));
  }

  /// Rewind every slab bump to zero. Does not free device memory.
  void Reset() noexcept {
    for (auto& entry : slabs_) {
      entry.offset = 0;
    }
    ClearStageStats();
  }

  /**
   * @brief Free device slabs. Caller must GPU-synchronize if kernels still read them.
   *
   * Develop scratch is released after SensorDevelop, not kept for later frames.
   */
  void ReleaseDeviceMemory() noexcept {
    const auto capacity = capacity_bytes();
    if (capacity > 0 && ShouldTraceGpuPoolAlloc(capacity)) {
      std::fprintf(stderr, "[GPU_POOL] transient release %.1f MiB\n", GpuPoolMiB(capacity));
      if constexpr (requires(const Backend& backend) { backend.QueryDeviceMemory(); }) {
        PrintGpuDeviceMemory(backend_->QueryDeviceMemory());
      }
    }
    ResetSlab();
  }

  [[nodiscard]] auto CaptureMark() const -> Mark {
    Mark mark;
    mark.reserve(slabs_.size());
    for (const auto& entry : slabs_) {
      mark.push_back(entry.offset);
    }
    return mark;
  }

  void RewindToMark(const Mark& mark) noexcept {
    for (std::size_t i = 0; i < slabs_.size(); ++i) {
      slabs_[i].offset = i < mark.size() ? mark[i] : 0;
    }
  }

  [[nodiscard]] auto capacity_bytes() const noexcept -> std::size_t {
    std::size_t total = 0;
    for (const auto& entry : slabs_) {
      total += entry.size;
    }
    return total;
  }
  [[nodiscard]] auto used_bytes() const noexcept -> std::size_t {
    std::size_t total = 0;
    for (const auto& entry : slabs_) {
      total += entry.offset;
    }
    return total;
  }
  [[nodiscard]] auto remaining_bytes() const noexcept -> std::size_t {
    std::size_t total = 0;
    for (const auto& entry : slabs_) {
      total += entry.size > entry.offset ? entry.size - entry.offset : 0;
    }
    return total;
  }
  [[nodiscard]] auto slab_count() const noexcept -> std::size_t { return slabs_.size(); }
  [[nodiscard]] auto base() const noexcept -> void* {
    return slabs_.empty() ? nullptr : slabs_.front().slab.DevicePointer();
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return used_bytes() == 0; }
  [[nodiscard]] auto Snapshot() const -> TransientArenaSnapshot {
    TransientArenaSnapshot snap;
    snap.requested_bytes    = requested_bytes_;
    snap.padding_bytes      = padding_bytes_;
    snap.used_bytes         = used_bytes();
    snap.capacity_bytes    = capacity_bytes();
    snap.unused_tail_bytes = remaining_bytes();
    snap.slab_count        = slabs_.size();
    snap.largest_allocation = largest_allocation_;
    snap.grow_count        = grow_count_;
    return snap;
  }

 private:
  struct SlabEntry {
    typename Backend::Slab slab{};
    std::size_t            size   = 0;
    std::size_t            offset = 0;
  };

  static auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  [[nodiscard]] auto HasLiveAllocations() const noexcept -> bool {
    for (const auto& entry : slabs_) {
      if (entry.offset != 0) {
        return true;
      }
    }
    return false;
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

  auto QueryMaxTransientBytes() const -> std::size_t {
    if (backend_ == nullptr) {
      return (std::numeric_limits<std::size_t>::max)();
    }
    if constexpr (requires(const Backend& backend) { backend.MaxTransientBytes(); }) {
      const auto cap = backend_->MaxTransientBytes();
      if (cap != 0) {
        return cap;
      }
    }
    return (std::numeric_limits<std::size_t>::max)();
  }

  auto SlabBytesForRequest(std::size_t request) const -> std::size_t {
    const auto max_slab = QueryMaxSlabBytes();
    std::size_t size     = kMinSlabBytes;
    if (request > kMinSlabBytes && request <= kSlabQuantumBytes) {
      size = kSlabQuantumBytes;
    } else if (request > kSlabQuantumBytes) {
      size = AlignUp(request, kSlabQuantumBytes);
    }
    if (size > max_slab) {
      size = max_slab;
    }
    if (size < request) {
      size = request;
    }
    return size;
  }

  auto TryAllocateIn(SlabEntry& entry, std::size_t bytes, std::size_t alignment) -> void* {
    const auto aligned = AlignUp(entry.offset, alignment);
    if (aligned < entry.offset) {
      return nullptr;
    }
    if (bytes > entry.size || aligned > entry.size - bytes) {
      return nullptr;
    }
    padding_bytes_ += aligned - entry.offset;
    entry.offset = aligned + bytes;
    return static_cast<char*>(entry.slab.DevicePointer()) + aligned;
  }

  void NoteAllocation(std::size_t bytes) noexcept {
    requested_bytes_ += bytes;
    if (bytes > largest_allocation_) {
      largest_allocation_ = bytes;
    }
  }

  void AppendSlab(std::size_t bytes) {
    const auto max_transient = QueryMaxTransientBytes();
    const auto current        = capacity_bytes();
    if (bytes > max_transient || current > max_transient - bytes) {
      throw std::runtime_error(FormatLimitError(
          "TransientBufferArena: allocation would exceed transient budget", bytes));
    }
    if (backend_ == nullptr) {
      throw std::runtime_error("TransientBufferArena::Allocate: backend is missing");
    }
    slabs_.push_back(SlabEntry{backend_->CreateSlab(bytes), bytes, 0});
  }

  void EnsureCapacity(std::size_t bytes) {
    // Drop unused slabs first. Creating the replacement before release would
    // keep both working sets alive and double the VRAM occupancy.
    ResetSlab();
    if (bytes == 0) {
      return;
    }
    const auto max_slab  = QueryMaxSlabBytes();
    std::size_t remaining = bytes;
    while (remaining > 0) {
      const auto chunk = remaining > max_slab ? max_slab : remaining;
      AppendSlab(chunk);
      remaining -= chunk;
    }
  }

  void ClearStageStats() noexcept {
    requested_bytes_    = 0;
    padding_bytes_      = 0;
    largest_allocation_ = 0;
    grow_count_         = 0;
  }

  void ResetSlab() noexcept {
    slabs_.clear();
    ClearStageStats();
  }

  auto FormatLimitError(const char* prefix, std::size_t requested) const -> std::string {
    char buffer[768];
    std::snprintf(buffer, sizeof(buffer),
                  "%s: requested=%zu used=%zu capacity=%zu slabs=%zu largest=%zu padding=%zu "
                  "unused_tail=%zu grow=%zu",
                  prefix, requested, used_bytes(), capacity_bytes(), slabs_.size(),
                  largest_allocation_, padding_bytes_, remaining_bytes(), grow_count_);
    std::string message = buffer;
    if constexpr (requires(const Backend& backend) { backend.QueryDeviceMemory(); }) {
      if (backend_ != nullptr) {
        const auto memory = backend_->QueryDeviceMemory();
        if (memory.valid) {
          char extra[256];
          std::snprintf(extra, sizeof(extra), " device_free=%zu device_total=%zu",
                        memory.free_bytes, memory.total_bytes);
          message += extra;
        }
      }
    }
    return message;
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
    slabs_               = std::move(other.slabs_);
    requested_bytes_     = other.requested_bytes_;
    padding_bytes_       = other.padding_bytes_;
    largest_allocation_ = other.largest_allocation_;
    grow_count_          = other.grow_count_;
    other.requested_bytes_     = 0;
    other.padding_bytes_       = 0;
    other.largest_allocation_ = 0;
    other.grow_count_          = 0;
  }

  std::optional<Backend> owned_;
  Backend*               backend_            = nullptr;
  std::vector<SlabEntry> slabs_;
  std::size_t            requested_bytes_     = 0;
  std::size_t            padding_bytes_      = 0;
  std::size_t            largest_allocation_ = 0;
  std::size_t            grow_count_         = 0;
};

}  // namespace alcedo
