//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"

namespace alcedo::cuda::nn {

// 2-D transposed convolution (f32, NCHW) for demosaicnet Bayer unpack.
//
// Weight layout matches PyTorch ConvTranspose2d:
//   [in_channels, out_channels / groups, kH, kW] on device.
// Bias is length out_channels (optional).
//
// Demosaicnet unpack_mosaick: in=12, out=3, k=2, s=2, pad=0, groups=3,
// dilation=1, output_padding=0. Weight shape [12, 1, 2, 2].
//
// Pure CUDA Runtime (cudart only). No cuBLAS / cuDNN.
struct ConvTranspose2dParams {
  int          in_channels    = 0;
  int          out_channels   = 0;
  int          kH             = 1;
  int          kW             = 1;
  int          sH             = 1;
  int          sW             = 1;
  int          padH           = 0;
  int          padW           = 0;
  int          output_padH    = 0;
  int          output_padW    = 0;
  int          dilation       = 1;  // isotropic
  int          groups         = 1;
  const float* weight         = nullptr;  // [Cin, Cout/groups, kH, kW]
  const float* bias           = nullptr;  // optional, length out_channels
};

// PyTorch default ConvTranspose2d spatial size (output_padding applied):
//   (in - 1)*stride - 2*pad + dilation*(kernel - 1) + output_padding + 1
[[nodiscard]] inline auto ConvTranspose2dOutputSize(int input_size, int pad, int dilation,
                                                    int kernel, int stride, int output_padding)
    -> int {
  if (input_size < 0 || pad < 0 || dilation < 1 || kernel < 1 || stride < 1 ||
      output_padding < 0) {
    return -1;
  }
  if (output_padding >= stride) {
    // PyTorch constraint: output_padding < stride (or < dilation if dilation > stride).
    // Reject here so callers get a clear failure rather than a silent wrong size.
    return -1;
  }
  return (input_size - 1) * stride - 2 * pad + dilation * (kernel - 1) + output_padding + 1;
}

[[nodiscard]] inline auto ConvTranspose2dOutputHeight(int in_h, const ConvTranspose2dParams& p)
    -> int {
  return ConvTranspose2dOutputSize(in_h, p.padH, p.dilation, p.kH, p.sH, p.output_padH);
}

[[nodiscard]] inline auto ConvTranspose2dOutputWidth(int in_w, const ConvTranspose2dParams& p)
    -> int {
  return ConvTranspose2dOutputSize(in_w, p.padW, p.dilation, p.kW, p.sW, p.output_padW);
}

// ConvTranspose2d (+ optional bias). Input and output must be contiguous rank-4 NCHW.
// Output spatial size must match ConvTranspose2dOutputHeight/Width.
//
// `workspace` is reserved for future scratch-backed paths. Current kernels are
// direct and do not allocate; when a non-null workspace is passed the hot path
// still performs no cudaMalloc.
void ConvTranspose2d(const DeviceTensor& input, DeviceTensor& output,
                     const ConvTranspose2dParams& params, cudaStream_t stream = nullptr,
                     WorkspacePool* workspace = nullptr);

}  // namespace alcedo::cuda::nn
