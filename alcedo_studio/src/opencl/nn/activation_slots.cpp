//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "opencl/nn/activation_slots.hpp"

namespace alcedo::opencl::nn {

void ActivationSlots::EnsureSlotBytes(const std::size_t slot_bytes) {
  if (slot_bytes == 0) {
    return;
  }
  if (slot_bytes <= slot_bytes_ && !slot_a_.empty() && !slot_b_.empty()) {
    return;
  }
  slot_a_.EnsureBytes(slot_bytes);
  slot_b_.EnsureBytes(slot_bytes);
  slot_bytes_ = slot_bytes;
  ++allocation_generation_;
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL