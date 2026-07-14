//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <utility>

#include "opencl/nn/common.hpp"
#include "opencl/nn/tensor_view.hpp"
#include "opencl/opencl_context.hpp"

namespace alcedo::opencl::nn {

// Byte-ranged view into a WorkspacePool base buffer (no extra cl_mem).
struct WorkspaceSlice {
  cl_mem      parent      = nullptr;
  std::size_t byte_offset = 0;
  std::size_t byte_size   = 0;

  [[nodiscard]] auto empty() const noexcept -> bool {
    return parent == nullptr || byte_size == 0;
  }
};

// RAII clCreateSubBuffer over a WorkspaceSlice. For kernels that take a cl_mem
// base without a byte-offset argument. DemosaicNet product path uses dedicated
// ActivationSlots instead; keep SubBuffer for generic NN primitives/tests only.
class SubBuffer {
 public:
  SubBuffer() = default;
  explicit SubBuffer(const WorkspaceSlice& slice);

  SubBuffer(const SubBuffer&)            = delete;
  SubBuffer& operator=(const SubBuffer&) = delete;

  SubBuffer(SubBuffer&& other) noexcept : buffer_(other.buffer_) { other.buffer_ = nullptr; }

  auto operator=(SubBuffer&& other) noexcept -> SubBuffer& {
    if (this != &other) {
      Reset();
      buffer_       = other.buffer_;
      other.buffer_ = nullptr;
    }
    return *this;
  }

  ~SubBuffer() { Reset(); }

  [[nodiscard]] auto get() const noexcept -> cl_mem { return buffer_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return buffer_ == nullptr; }

  void Reset() noexcept;

 private:
  cl_mem buffer_ = nullptr;
};

// Grow-only OpenCL device workspace for ephemeral CNN intermediates.
//
// Design (steady-state forward must not call clCreateBuffer):
// - One device slab owned by the pool.
// - Bump allocator: Allocate advances an offset; Reset() rewinds to zero.
// - Capacity grows only when the bump offset is zero (no live pointers), or via
//   an explicit Reserve() before a forward.
// - allocation_generation() increments only when a new parent cl_mem is allocated.
//
// Not thread-safe. One pool per Neural decode or serialize access.
class WorkspacePool {
 public:
  static constexpr std::size_t kDefaultAlignment = 256;

  WorkspacePool() = default;

  explicit WorkspacePool(std::size_t initial_capacity_bytes) { Reserve(initial_capacity_bytes); }

  WorkspacePool(const WorkspacePool&)            = delete;
  WorkspacePool& operator=(const WorkspacePool&) = delete;

  WorkspacePool(WorkspacePool&& other) noexcept
      : buffer_(other.buffer_),
        capacity_(other.capacity_),
        offset_(other.offset_),
        allocation_generation_(other.allocation_generation_) {
    other.buffer_                = nullptr;
    other.capacity_              = 0;
    other.offset_                = 0;
    other.allocation_generation_ = 0;
  }

  auto operator=(WorkspacePool&& other) noexcept -> WorkspacePool& {
    if (this != &other) {
      FreeDevice();
      buffer_                      = other.buffer_;
      capacity_                    = other.capacity_;
      offset_                      = other.offset_;
      allocation_generation_       = other.allocation_generation_;
      other.buffer_                = nullptr;
      other.capacity_              = 0;
      other.offset_                = 0;
      other.allocation_generation_ = 0;
    }
    return *this;
  }

  ~WorkspacePool() { FreeDevice(); }

  // Ensure capacity >= bytes. Grow-only. Fails if bump allocations are live.
  void Reserve(std::size_t bytes);

  // Bump-allocate `bytes` (0 → empty slice). Grows only when the pool is empty.
  [[nodiscard]] auto Allocate(std::size_t bytes, std::size_t alignment = kDefaultAlignment)
      -> WorkspaceSlice;

  // Allocate a contiguous NHWC4 region as a WorkspaceSlice + view metadata.
  // The view.buffer remains the parent base; use SubBuffer(slice) before binding
  // to kernels that do not accept byte offsets.
  [[nodiscard]] auto AllocateNhwc4(int batch, int height, int width, int logical_channels,
                                   std::size_t alignment = kDefaultAlignment)
      -> std::pair<WorkspaceSlice, Nhwc4TensorView>;

  [[nodiscard]] auto AllocateNhwc4Blocked(int batch, int height, int width, int logical_channels,
                                          int channel_blocks,
                                          std::size_t alignment = kDefaultAlignment)
      -> std::pair<WorkspaceSlice, Nhwc4TensorView>;

  void Reset() noexcept { offset_ = 0; }

  void Rewind(std::size_t mark_bytes) {
    if (mark_bytes > offset_) {
      throw std::runtime_error("WorkspacePool::Rewind: mark is past current offset");
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
  [[nodiscard]] auto base() const noexcept -> cl_mem { return buffer_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return offset_ == 0; }

  // Increments only when a new parent cl_mem is created.
  [[nodiscard]] auto allocation_generation() const noexcept -> std::uint64_t {
    return allocation_generation_;
  }

 private:
  static auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  void FreeDevice() noexcept;

  cl_mem        buffer_                = nullptr;
  std::size_t   capacity_              = 0;
  std::size_t   offset_                = 0;
  std::uint64_t allocation_generation_ = 0;
};

// RAII nested scope: rewinds the pool offset to the mark taken at construction.
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

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
