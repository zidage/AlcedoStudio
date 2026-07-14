//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <memory>

#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"
#include "opencl/nn/activation_slots.hpp"

namespace alcedo {

// Device-side product execution for the fixed OpenCL student modules.
//
// The input is an already phase-aligned and CFA-period-trimmed HWC3 linear
// mosaic. Each job is packed with reflect-101 addressing and signed gamma
// directly into the network's first persistent NHWC4 activation, then
// assembled directly into aligned_rgb_hwc.
// All commands are queued on one in-order queue; this class intentionally does
// not wait, finish, or read back inside (or after) its tile loop. Keep this
// executor and the activation slots alive until the owner performs the final
// queue wait at the Neural-stage boundary.
struct OpenClDemosaicNetTiledDispatch {
  cl_mem           input_aligned_hwc  = nullptr;
  cl_mem           output_aligned_hwc = nullptr;
  int              aligned_width      = 0;
  int              aligned_height     = 0;
  cl_command_queue queue              = nullptr;
};

struct OpenClDemosaicNetTiledResult {
  std::size_t tile_count    = 0;
  int         output_width  = 0;
  int         output_height = 0;
};

class OpenClDemosaicNetTiledExecutor {
 public:
  struct Impl;

  OpenClDemosaicNetTiledExecutor();
  ~OpenClDemosaicNetTiledExecutor();

  OpenClDemosaicNetTiledExecutor(const OpenClDemosaicNetTiledExecutor&)                    = delete;
  auto operator=(const OpenClDemosaicNetTiledExecutor&) -> OpenClDemosaicNetTiledExecutor& = delete;
  OpenClDemosaicNetTiledExecutor(OpenClDemosaicNetTiledExecutor&&) noexcept;
  auto operator=(OpenClDemosaicNetTiledExecutor&&) noexcept -> OpenClDemosaicNetTiledExecutor&;

  [[nodiscard]] auto EnqueueBayer(const OpenClBayerDemosaicNet&         module,
                                  opencl::nn::ActivationSlots&          activation_slots,
                                  const OpenClDemosaicNetTiledDispatch& dispatch)
      -> OpenClDemosaicNetTiledResult;

  [[nodiscard]] auto EnqueueXTrans(const OpenClXTransDemosaicNet&        module,
                                   opencl::nn::ActivationSlots&          activation_slots,
                                   const OpenClDemosaicNetTiledDispatch& dispatch)
      -> OpenClDemosaicNetTiledResult;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo

#endif  // HAVE_OPENCL
