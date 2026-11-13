//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <initializer_list>

namespace alcedo {

/**
 * @brief Wait for recorded GPU work, then destroy the slabs of @p pointers.
 *
 * Release is keyed by last GPU use, not by C++ scope exit. Tile buffers that
 * are still reused must not be passed until that reuse is finished.
 */
template <class Device>
void ReleaseTransientSlabsAfterGpuLastUse(Device& device, std::initializer_list<void*> pointers) {
  bool any = false;
  for (void* pointer : pointers) {
    if (pointer != nullptr) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  device.Workspace().Device().SynchronizeRecordedWork(device.CommandContext());
  auto& arena = device.Workspace().TransientBuffers();
  for (void* pointer : pointers) {
    if (pointer != nullptr) {
      arena.ReleaseSlabContaining(pointer);
    }
  }
}

}  // namespace alcedo
