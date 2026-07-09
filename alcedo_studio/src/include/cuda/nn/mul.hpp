//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Element-wise multiply: out = a * b, f32 only.
//
// Implementation strategy (bandwidth-bound, same as ReLU):
// - Contiguous tensors: vectorized float4 loads/stores + scalar tail.
// - Non-contiguous: strided kernel with multi-index decode.
//
// Shapes of a, b, and out must match. Inplace forms write into the first operand
// (a *= b). All APIs are stream-aware; pass nullptr for the default stream.

void Mul(const float* a, const float* b, float* out, std::int64_t n,
         cudaStream_t stream = nullptr);
void MulInplace(float* a, const float* b, std::int64_t n, cudaStream_t stream = nullptr);

void Mul(const DeviceTensor& a, const DeviceTensor& b, DeviceTensor& out,
         cudaStream_t stream = nullptr);
void MulInplace(DeviceTensor& a, const DeviceTensor& b, cudaStream_t stream = nullptr);

}  // namespace alcedo::cuda::nn
