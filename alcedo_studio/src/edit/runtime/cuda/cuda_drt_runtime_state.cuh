//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/GPU_kernels/param.cuh"
#include "edit/operators/op_base.hpp"

namespace alcedo {

/** CUDA-only resolved DRT data retained for the lifetime of one render device. */
class CudaDrtRuntimeState {
 public:
  ~CudaDrtRuntimeState() { gpu_params.to_output_params_.Reset(); }

  OperatorParams    cpu_params{};
  GPUOperatorParams gpu_params{};
};

}  // namespace alcedo
