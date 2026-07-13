// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

namespace alcedo::cuda::nn {

// One-time host prepack from PyTorch OIHW to CUTLASS KRSC.
void TransformConv2d3x3WeightsCutlassKrsc(const float* src_oihw, int channels, float* dst_krsc);

// CUTLASS 3.9.2 FP32 SIMT implicit-GEMM for the persistent NHWC C=32 X-Trans trunk.
// Input/output are tightly packed NHWC; weights are KRSC and bias is length C.
void Conv2d3x3NhwcCutlassBiasRelu(const float* input_nhwc, float* output_nhwc,
                                  const float* weight_krsc, const float* bias, int batch,
                                  int height, int width, int channels,
                                  cudaStream_t stream = nullptr);

}  // namespace alcedo::cuda::nn
