//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {

/**
 * @brief Records a native GPU timestamp pair for the current preview pass or sub-stage.
 *
 * No-op when preview timing is Off or the device has no timestamp API. Begin and End
 * only enqueue native markers; elapsed time is read after submission completion.
 *
 * @tparam Device Render device exposing optional BeginGpuWorkSample / EndGpuWorkSample.
 */
template <class Device>
class GpuWorkSample {
 public:
  explicit GpuWorkSample(Device& device) : device_(&device) {
    if (!diag::PreviewPerformanceEnabled()) {
      device_ = nullptr;
      return;
    }
    if constexpr (requires(Device& candidate) { candidate.BeginGpuWorkSample(); }) {
      device.BeginGpuWorkSample();
      active_ = true;
    } else {
      device_ = nullptr;
    }
  }

  ~GpuWorkSample() {
    if (!active_ || device_ == nullptr) {
      return;
    }
    if constexpr (requires(Device& candidate) { candidate.EndGpuWorkSample(); }) {
      device_->EndGpuWorkSample();
    }
  }

  GpuWorkSample(const GpuWorkSample&)                    = delete;
  auto operator=(const GpuWorkSample&) -> GpuWorkSample& = delete;

 private:
  Device* device_ = nullptr;
  bool    active_ = false;
};

}  // namespace alcedo
