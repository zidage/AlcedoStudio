//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"

namespace alcedo::cuda::nn {

// 2-D convolution parameters for demosaicnet-style CNNs (f32, NCHW).
//
// Weight layout matches PyTorch Conv2d: OIHW [out_channels, in_channels/groups, kH, kW]
// on device. Bias is length out_channels (optional).
//
// Demosaicnet constraints for the first milestone:
// - padH/padW = 0 (valid convolution)
// - groups = 1 (grouped transpose is Phase 3)
// - dilation = 1
// - k ∈ {1,2,3}, s ∈ {1,2} cover all demosaicnet Conv2d layers
//
// Implementation is pure CUDA Runtime (cudart only). No cuBLAS / cuDNN link —
// packaging ships only cudart64_*.dll for CUDA.
struct Conv2dParams {
  int          in_channels  = 0;
  int          out_channels = 0;
  int          kH           = 1;
  int          kW           = 1;
  int          sH           = 1;
  int          sW           = 1;
  int          padH         = 0;
  int          padW         = 0;
  int          dilation     = 1;  // isotropic; same for H and W
  int          groups       = 1;
  const float* weight       = nullptr;  // OIHW device pointer
  const float* bias         = nullptr;  // optional, length out_channels
};

// PyTorch / demosaicnet spatial output size:
//   floor((in + 2*pad - dilation*(k-1) - 1) / stride) + 1
[[nodiscard]] inline auto Conv2dOutputSize(int input_size, int pad, int dilation, int kernel,
                                           int stride) -> int {
  if (input_size < 0 || pad < 0 || dilation < 1 || kernel < 1 || stride < 1) {
    return -1;
  }
  return (input_size + 2 * pad - dilation * (kernel - 1) - 1) / stride + 1;
}

[[nodiscard]] inline auto Conv2dOutputHeight(int in_h, const Conv2dParams& p) -> int {
  return Conv2dOutputSize(in_h, p.padH, p.dilation, p.kH, p.sH);
}

[[nodiscard]] inline auto Conv2dOutputWidth(int in_w, const Conv2dParams& p) -> int {
  return Conv2dOutputSize(in_w, p.padW, p.dilation, p.kW, p.sW);
}

// Conv2d (+ optional bias). Input and output must be contiguous rank-4 NCHW.
// Output spatial size must match Conv2dOutputHeight/Width.
//
// `workspace` is reserved for scratch-backed paths (e.g. im2col). Current kernels
// are direct / implicit and do not allocate; when a non-null workspace is passed,
// the hot path still performs no cudaMalloc (steady-state requirement).
void Conv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
            cudaStream_t stream = nullptr, WorkspacePool* workspace = nullptr);

// Fused Conv2d + bias + ReLU. Bias may be null (then only ReLU is applied after conv).
void Conv2dBiasRelu(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                    cudaStream_t stream = nullptr, WorkspacePool* workspace = nullptr);

}  // namespace alcedo::cuda::nn
