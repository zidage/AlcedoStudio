//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>

#include "opencl/nn/device_buffer.hpp"

namespace alcedo::opencl::nn {

// Two dedicated grow-only ping-pong activation buffers for fixed DemosaicNet
// forwards. Prefer this over WorkspacePool + SubBuffer on the Neural hot path:
// kernels bind full cl_mem bases with no per-tile sub-buffer create/release.
//
// Not thread-safe. One active Neural decode may use a given instance at a time.
class ActivationSlots {
 public:
  ActivationSlots() = default;

  ActivationSlots(const ActivationSlots&)            = delete;
  ActivationSlots& operator=(const ActivationSlots&) = delete;

  ActivationSlots(ActivationSlots&&) noexcept            = default;
  ActivationSlots& operator=(ActivationSlots&&) noexcept = default;

  // Grow-only: each slot is ensured to hold at least `slot_bytes`.
  // No-op when capacity is already sufficient (hot path after warm-up).
  void EnsureSlotBytes(std::size_t slot_bytes);

  [[nodiscard]] auto slot_a() const noexcept -> cl_mem { return slot_a_.get(); }
  [[nodiscard]] auto slot_b() const noexcept -> cl_mem { return slot_b_.get(); }
  [[nodiscard]] auto slot_bytes() const noexcept -> std::size_t { return slot_bytes_; }
  [[nodiscard]] auto empty() const noexcept -> bool {
    return slot_a_.empty() || slot_b_.empty() || slot_bytes_ == 0;
  }

  // Increments only when either physical cl_mem is reallocated.
  [[nodiscard]] auto allocation_generation() const noexcept -> std::uint64_t {
    return allocation_generation_;
  }

 private:
  DeviceBuffer  slot_a_;
  DeviceBuffer  slot_b_;
  std::size_t   slot_bytes_            = 0;
  std::uint64_t allocation_generation_ = 0;
};

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
