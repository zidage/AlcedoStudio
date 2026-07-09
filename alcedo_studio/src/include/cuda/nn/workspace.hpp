//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/common.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Grow-only device workspace for ephemeral CNN intermediates.
//
// Design (steady-state forward must not call cudaMalloc):
// - One (or, after Reserve, a larger) device slab owned by the pool.
// - Bump allocator: Allocate advances an offset; Reset() rewinds to zero.
// - Nested scopes: WorkspaceScope captures the offset and rewinds on destroy
//   (LIFO). Prefer Reset() once at the end of each model forward.
// - Capacity grows only when the bump offset is zero (no live pointers), or via
//   an explicit Reserve() before a forward. Callers should Reserve the peak
//   workspace once at model / tile setup.
//
// Not thread-safe. One pool per stream or serialize access.
class WorkspacePool {
 public:
  // Default CUDA alignment for activations (good for float4 / vector paths).
  static constexpr std::size_t kDefaultAlignment = 256;

  WorkspacePool() = default;

  explicit WorkspacePool(std::size_t initial_capacity_bytes) { Reserve(initial_capacity_bytes); }

  WorkspacePool(const WorkspacePool&)            = delete;
  WorkspacePool& operator=(const WorkspacePool&) = delete;

  WorkspacePool(WorkspacePool&& other) noexcept
      : base_(other.base_), capacity_(other.capacity_), offset_(other.offset_) {
    other.base_     = nullptr;
    other.capacity_ = 0;
    other.offset_   = 0;
  }

  auto operator=(WorkspacePool&& other) noexcept -> WorkspacePool& {
    if (this != &other) {
      FreeDevice();
      base_           = other.base_;
      capacity_       = other.capacity_;
      offset_         = other.offset_;
      other.base_     = nullptr;
      other.capacity_ = 0;
      other.offset_   = 0;
    }
    return *this;
  }

  ~WorkspacePool() { FreeDevice(); }

  // Ensure capacity >= bytes. Grow-only. Fails if bump allocations are live
  // (offset_ != 0), because live pointers would dangle after reallocation.
  void Reserve(std::size_t bytes) {
    if (bytes <= capacity_) {
      return;
    }
    if (offset_ != 0) {
      throw std::runtime_error(
          "WorkspacePool::Reserve: cannot grow while allocations are live; "
          "Reset() first or Reserve peak size before Allocate");
    }
    void* new_base = nullptr;
    CheckCuda(::cudaMalloc(&new_base, bytes), "WorkspacePool::Reserve");
    if (base_ != nullptr) {
      ::cudaFree(base_);
    }
    base_     = new_base;
    capacity_ = bytes;
  }

  // Bump-allocate `bytes` (may be 0 → returns nullptr). Grows capacity only when
  // the pool is empty (offset_ == 0). Otherwise throws if capacity is insufficient
  // so callers learn to Reserve peak usage up front.
  [[nodiscard]] auto Allocate(std::size_t bytes, std::size_t alignment = kDefaultAlignment)
      -> void* {
    if (bytes == 0) {
      return nullptr;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
      throw std::runtime_error("WorkspacePool::Allocate: alignment must be a power of two");
    }

    const std::size_t aligned_offset = AlignUp(offset_, alignment);
    const std::size_t end            = aligned_offset + bytes;

    if (end > capacity_) {
      if (offset_ != 0) {
        throw std::runtime_error(
            "WorkspacePool::Allocate: insufficient capacity with live allocations; "
            "call Reserve() with the peak workspace size before forward");
      }
      // Empty pool: grow with modest headroom so repeated small allocs do not thrash.
      std::size_t new_cap = capacity_ == 0 ? end : capacity_;
      while (new_cap < end) {
        const std::size_t doubled = new_cap > (std::size_t{1} << 62) ? end : new_cap * 2;
        new_cap                   = doubled < end ? end : doubled;
      }
      Reserve(new_cap);
      // offset_ still 0; recompute (aligned_offset was 0).
      offset_ = bytes;
      return base_;
    }

    offset_ = end;
    return static_cast<char*>(base_) + aligned_offset;
  }

  // Typed convenience: allocate `count` elements of T (default-aligned).
  template <typename T>
  [[nodiscard]] auto Allocate(std::size_t count, std::size_t alignment = kDefaultAlignment) -> T* {
    return static_cast<T*>(Allocate(count * sizeof(T), alignment));
  }

  [[nodiscard]] auto AllocateFloats(std::size_t count,
                                    std::size_t alignment = kDefaultAlignment) -> float* {
    return Allocate<float>(count, alignment);
  }

  // Contiguous NCHW (or any rank) tensor backed by the bump allocator.
  [[nodiscard]] auto AllocateTensor(std::initializer_list<std::int64_t> dims,
                                    std::size_t alignment = kDefaultAlignment) -> DeviceTensor {
    return AllocateTensor(dims.begin(), static_cast<int>(dims.size()), alignment);
  }

  [[nodiscard]] auto AllocateTensor(const std::vector<std::int64_t>& dims,
                                    std::size_t alignment = kDefaultAlignment) -> DeviceTensor {
    return AllocateTensor(dims.data(), static_cast<int>(dims.size()), alignment);
  }

  // Rewind bump pointer to zero. Does not free device memory.
  void Reset() noexcept { offset_ = 0; }

  // Rewind to a previously recorded mark (from used_bytes()). LIFO only.
  void Rewind(std::size_t mark_bytes) {
    if (mark_bytes > offset_) {
      throw std::runtime_error("WorkspacePool::Rewind: mark is past current offset");
    }
    offset_ = mark_bytes;
  }

  // Non-throwing rewind for RAII scopes (clamps to current offset).
  void RewindUnchecked(std::size_t mark_bytes) noexcept {
    offset_ = mark_bytes < offset_ ? mark_bytes : offset_;
  }

  [[nodiscard]] auto capacity_bytes() const noexcept -> std::size_t { return capacity_; }
  [[nodiscard]] auto used_bytes() const noexcept -> std::size_t { return offset_; }
  [[nodiscard]] auto remaining_bytes() const noexcept -> std::size_t {
    return capacity_ > offset_ ? capacity_ - offset_ : 0;
  }
  [[nodiscard]] auto base() const noexcept -> void* { return base_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return offset_ == 0; }

 private:
  [[nodiscard]] auto AllocateTensor(const std::int64_t* dims, int rank,
                                    std::size_t alignment) -> DeviceTensor {
    std::int64_t numel = 1;
    for (int i = 0; i < rank; ++i) {
      if (dims[i] < 0) {
        throw std::runtime_error("WorkspacePool::AllocateTensor: negative dimension");
      }
      numel *= dims[i];
    }
    float* ptr = AllocateFloats(static_cast<std::size_t>(numel), alignment);
    return DeviceTensor::Contiguous(ptr, dims, rank);
  }

  static auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void FreeDevice() noexcept {
    if (base_ != nullptr) {
      ::cudaFree(base_);
      base_ = nullptr;
    }
    capacity_ = 0;
    offset_   = 0;
  }

  void*       base_     = nullptr;
  std::size_t capacity_ = 0;
  std::size_t offset_   = 0;
};

// RAII nested scope: rewinds the pool offset to the mark taken at construction.
// Allocations made inside the scope become invalid when the scope ends.
class WorkspaceScope {
 public:
  explicit WorkspaceScope(WorkspacePool& pool) : pool_(&pool), mark_(pool.used_bytes()) {}

  WorkspaceScope(const WorkspaceScope&)            = delete;
  WorkspaceScope& operator=(const WorkspaceScope&) = delete;

  WorkspaceScope(WorkspaceScope&& other) noexcept : pool_(other.pool_), mark_(other.mark_) {
    other.pool_ = nullptr;
  }

  auto operator=(WorkspaceScope&& other) noexcept -> WorkspaceScope& {
    if (this != &other) {
      Release();
      pool_       = other.pool_;
      mark_       = other.mark_;
      other.pool_ = nullptr;
    }
    return *this;
  }

  ~WorkspaceScope() { Release(); }

  // Early rewind (idempotent).
  void Release() noexcept {
    if (pool_ != nullptr) {
      pool_->RewindUnchecked(mark_);
      pool_ = nullptr;
    }
  }

  [[nodiscard]] auto mark() const noexcept -> std::size_t { return mark_; }

 private:
  WorkspacePool* pool_ = nullptr;
  std::size_t    mark_ = 0;
};

}  // namespace alcedo::cuda::nn
