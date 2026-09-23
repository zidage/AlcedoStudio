//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/utils/color_utils.hpp"
#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"

namespace alcedo {

/**
 * @brief CUDA DRT parameter block and the ACES lookup tables it references, owned by one render
 * device.
 *
 * `Pack` runs on the render thread when the DRT parameters are dirty. It is not thread-safe; the
 * owning `CudaRenderDevice` serializes access. The destructor releases the device tables.
 */
class CudaDrtRuntimeState {
 public:
  CudaDrtRuntimeState() = default;
  ~CudaDrtRuntimeState();
  CudaDrtRuntimeState(const CudaDrtRuntimeState&)                    = delete;
  auto operator=(const CudaDrtRuntimeState&) -> CudaDrtRuntimeState& = delete;

  /**
   * @brief Copy @p resolved into the CUDA parameter layout.
   *
   * ACES 2.0 uploads the four lookup tables only when the resolved host table differs from the
   * uploaded one; OpenDRT releases them. The returned block holds texture handles owned by this
   * state and stays valid until the next `Pack` or destruction.
   *
   * @throws std::runtime_error when an ACES 2.0 transform has no resolved tables.
   */
  auto Pack(const ColorUtils::TO_OUTPUT_Params& resolved) -> const CudaDrtGpuParams&;

 private:
  CudaDrtGpuParams params_{};
};

}  // namespace alcedo
