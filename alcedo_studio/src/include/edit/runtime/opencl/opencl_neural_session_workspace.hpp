//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
#include "opencl/nn/activation_slots.hpp"
#include "opencl/nn/device_buffer.hpp"

namespace alcedo {

/**
 * @brief Per-backend Neural tile executor, activation slots, and CFA table.
 *
 * Not process-static. Two OpenCL DAG devices (image A still tiling, image B
 * skipping or starting SensorDevelop) must not Reset each other's tile_output.
 * Weights stay in OpenClDemosaicNetModelCache.
 */
struct OpenClNeuralSessionWorkspace {
  OpenClDemosaicNetTiledExecutor executor;
  opencl::nn::ActivationSlots    slots;
  opencl::nn::DeviceBuffer       cfa_table;

  void                           Reset() {
    executor = {};
    slots    = {};
    cfa_table.Reset();
  }
};

}  // namespace alcedo

#endif
