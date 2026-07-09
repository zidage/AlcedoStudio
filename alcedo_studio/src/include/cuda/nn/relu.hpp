//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include <opencv2/core/cuda.hpp>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Element-wise ReLU: y = max(x, 0), f32 only.
//
// Implementation strategy (bandwidth-bound elementwise op):
// - Contiguous tensors: vectorized float4 loads/stores + scalar tail (coalesced, 16B transactions).
// - Non-contiguous / pitched GpuMat: 2D row kernel that honors per-row step (zero-copy).
// - Grid-stride launch so one binary handles tiny and multi-megapixel activations.
//
// All APIs are stream-aware. Pass nullptr for the default stream.

// Raw contiguous (or treat-as-flat) device pointers.
void Relu(const float* input, float* output, std::int64_t n, cudaStream_t stream = nullptr);
void ReluInplace(float* data, std::int64_t n, cudaStream_t stream = nullptr);

// Rank-agnostic tensor views. Contiguous tensors take the fast vectorized path.
// Input and output must share the same shape; strides may differ.
void Relu(const DeviceTensor& input, DeviceTensor& output, cudaStream_t stream = nullptr);
void ReluInplace(DeviceTensor& tensor, cudaStream_t stream = nullptr);

// Image / pipeline convenience over GpuMat (CV_32F, any channel count).
// Allocates output with matching size/type when needed. No channel transpose.
void Relu(const cv::cuda::GpuMat& input, cv::cuda::GpuMat& output,
          cv::cuda::Stream* stream = nullptr);
void ReluInplace(cv::cuda::GpuMat& image, cv::cuda::Stream* stream = nullptr);

}  // namespace alcedo::cuda::nn
