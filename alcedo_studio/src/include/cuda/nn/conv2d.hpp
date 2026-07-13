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

// Host-side one-time weight prepack for the Bayer persistent-NHWC 3×3 trunk.
// Input OIHW → [Cin, 3, 3, Cout]. Supports C=24 only (X-Trans C=32 uses CUTLASS KRSC).
void TransformConv2d3x3WeightsNhwc(const float* src_oihw, int in_channels, int out_channels,
                                   float* dst_ckco);
void Conv2d3x3NhwcBiasRelu(const float* input_nhwc, float* output_nhwc, const float* weight_ckco,
                           const float* bias, int batch, int height, int width, int channels,
                           cudaStream_t stream = nullptr);

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
// Dispatch (cudart only — no cuBLAS / cuDNN):
//   - 1×1 s=1 pad=0: tiled GEMM-style (spatial × Cout tiles, Cin strip SMEM);
//     exact small-Cout kernels for student residual/output (Cout 12 / 3)
//   - 2×2 s=2 pad=0: specialized pack_mosaick kernel
//   - 3×3 s=1 pad=0: multi-Cout tiled direct (input apron + weight SMEM)
//   - other shapes: generic direct fallback
//
// `workspace` is reserved for scratch-backed multi-pass paths. Current product
// kernels do not allocate; when a non-null workspace is passed, the hot path
// still performs no cudaMalloc.
void Conv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
            cudaStream_t stream = nullptr, WorkspacePool* workspace = nullptr);

// Fused Conv2d + bias + ReLU. Bias may be null (then only ReLU is applied after conv).
void Conv2dBiasRelu(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                    cudaStream_t stream = nullptr, WorkspacePool* workspace = nullptr);

// Describe the 3×3 s=1 product kernel family LaunchConv2d would select for the
// given channel shape. Does not launch work or synchronize. Returns false when
// the shape would not use a specialized 3×3 path.
struct Conv2d3x3KernelInfo {
  const char* name                   = nullptr;  // product path, e.g. "direct_tiled_24"
  int         cin                    = 0;
  int         cout                   = 0;
  int         num_regs               = 0;
  int         static_smem_bytes      = 0;
  int         max_dynamic_smem_bytes = 0;
  int         max_threads_per_block  = 0;
  int         threads_per_block      = 0;
  int         dynamic_smem_bytes     = 0;
};

[[nodiscard]] auto QueryConv2d3x3KernelInfo(int cin, int cout, Conv2d3x3KernelInfo* out) -> bool;

}  // namespace alcedo::cuda::nn
