//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/relu.hpp"

#include <cuda_runtime.h>

#include <opencv2/core/cuda_stream_accessor.hpp>

#include <stdexcept>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

// Grid-stride scalar path. Correct for any length; also cleans up float4 tails.
__global__ void ReluScalarKernel(const float* __restrict__ input, float* __restrict__ output,
                                 std::int64_t n) {
  const std::int64_t stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    output[i] = fmaxf(input[i], 0.0f);
  }
}

// Vectorized path: 16-byte load/store per thread iteration.
// Requires 16-byte aligned pointers and n4 complete float4 groups.
__global__ void ReluFloat4Kernel(const float4* __restrict__ input, float4* __restrict__ output,
                                 std::int64_t n4) {
  const std::int64_t stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n4;
       i += stride) {
    float4 v = input[i];
    v.x      = fmaxf(v.x, 0.0f);
    v.y      = fmaxf(v.y, 0.0f);
    v.z      = fmaxf(v.z, 0.0f);
    v.w      = fmaxf(v.w, 0.0f);
    output[i] = v;
  }
}

// Fixed-size metadata fits in the CUDA kernel parameter blob (no device alloc / sync).
struct StridedReluMeta {
  int          rank = 0;
  std::int64_t shape[kMaxTensorRank]       = {};
  std::int64_t in_strides[kMaxTensorRank]  = {};
  std::int64_t out_strides[kMaxTensorRank] = {};
};

// Pitched / non-contiguous logical layout: one thread per logical element.
// Decodes the multi-index from a flat logical index using input/output strides.
__global__ void ReluStridedKernel(const float* __restrict__ input, float* __restrict__ output,
                                  std::int64_t numel, StridedReluMeta meta) {
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t remaining  = linear;
    std::int64_t in_offset  = 0;
    std::int64_t out_offset = 0;
    // Row-major decode: last dimension varies fastest.
    for (int d = meta.rank - 1; d >= 0; --d) {
      const std::int64_t coord = remaining % meta.shape[d];
      remaining /= meta.shape[d];
      in_offset += coord * meta.in_strides[d];
      out_offset += coord * meta.out_strides[d];
    }
    output[out_offset] = fmaxf(input[in_offset], 0.0f);
  }
}

// 2D pitched row kernel for GpuMat: respects per-row byte step, no pack needed.
__global__ void ReluPitched2DKernel(const float* __restrict__ input, float* __restrict__ output,
                                    int rows, int width_floats, std::size_t in_step_bytes,
                                    std::size_t out_step_bytes) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width_floats || y >= rows) {
    return;
  }
  const auto* in_row =
      reinterpret_cast<const float*>(reinterpret_cast<const char*>(input) +
                                     static_cast<std::size_t>(y) * in_step_bytes);
  auto* out_row = reinterpret_cast<float*>(reinterpret_cast<char*>(output) +
                                           static_cast<std::size_t>(y) * out_step_bytes);
  out_row[x] = fmaxf(in_row[x], 0.0f);
}

auto IsDevicePointerAligned(const void* ptr, std::size_t alignment) -> bool {
  return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
}

void LaunchContiguousRelu(const float* input, float* output, std::int64_t n, cudaStream_t stream) {
  if (n <= 0) {
    return;
  }

  // Prefer float4 when both buffers are 16-byte aligned (cudaMalloc guarantees this).
  const bool can_vectorize = IsDevicePointerAligned(input, 16) && IsDevicePointerAligned(output, 16);
  if (can_vectorize && n >= 4) {
    const std::int64_t n4 = n / 4;
    const int          grid4 = ChooseGridSize(n4, kBlockSize);
    ReluFloat4Kernel<<<grid4, kBlockSize, 0, stream>>>(
        reinterpret_cast<const float4*>(input), reinterpret_cast<float4*>(output), n4);
    CheckCuda(cudaGetLastError(), "ReluFloat4Kernel launch");

    const std::int64_t tail = n - n4 * 4;
    if (tail > 0) {
      const std::int64_t offset = n4 * 4;
      const int          grid_tail = ChooseGridSize(tail, kBlockSize);
      ReluScalarKernel<<<grid_tail, kBlockSize, 0, stream>>>(input + offset, output + offset, tail);
      CheckCuda(cudaGetLastError(), "ReluScalarKernel tail launch");
    }
    return;
  }

  const int grid = ChooseGridSize(n, kBlockSize);
  ReluScalarKernel<<<grid, kBlockSize, 0, stream>>>(input, output, n);
  CheckCuda(cudaGetLastError(), "ReluScalarKernel launch");
}

void LaunchStridedRelu(const DeviceTensor& input, DeviceTensor& output, cudaStream_t stream) {
  const std::int64_t numel = input.Numel();
  if (numel <= 0) {
    return;
  }

  StridedReluMeta meta;
  meta.rank = input.rank;
  for (int i = 0; i < input.rank; ++i) {
    meta.shape[i]       = input.shape[i];
    meta.in_strides[i]  = input.strides[i];
    meta.out_strides[i] = output.strides[i];
  }

  const int grid = ChooseGridSize(numel, kBlockSize);
  ReluStridedKernel<<<grid, kBlockSize, 0, stream>>>(input.data, output.data, numel, meta);
  CheckCuda(cudaGetLastError(), "ReluStridedKernel launch");
}

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  if (stream == nullptr) {
    return nullptr;
  }
  return cv::cuda::StreamAccessor::getStream(*stream);
}

void ValidateSameShape(const DeviceTensor& a, const DeviceTensor& b) {
  if (a.rank != b.rank) {
    throw std::runtime_error("Relu: input/output rank mismatch");
  }
  for (int i = 0; i < a.rank; ++i) {
    if (a.shape[i] != b.shape[i]) {
      throw std::runtime_error("Relu: input/output shape mismatch");
    }
  }
}

}  // namespace

void Relu(const float* input, float* output, std::int64_t n, cudaStream_t stream) {
  if (n < 0) {
    throw std::runtime_error("Relu: negative element count");
  }
  if (n == 0) {
    return;
  }
  if (input == nullptr || output == nullptr) {
    throw std::runtime_error("Relu: null device pointer");
  }
  LaunchContiguousRelu(input, output, n, ResolveStream(stream));
}

void ReluInplace(float* data, std::int64_t n, cudaStream_t stream) {
  Relu(data, data, n, stream);
}

void Relu(const DeviceTensor& input, DeviceTensor& output, cudaStream_t stream) {
  if (input.data == nullptr || output.data == nullptr) {
    throw std::runtime_error("Relu: null DeviceTensor data");
  }
  ValidateSameShape(input, output);

  const std::int64_t numel = input.Numel();
  if (numel == 0) {
    return;
  }

  // Fast path: both contiguous → treat as flat buffers (NCHW, NHWC, 1D, … all OK).
  if (input.IsContiguous() && output.IsContiguous()) {
    LaunchContiguousRelu(input.data, output.data, numel, ResolveStream(stream));
    return;
  }

  LaunchStridedRelu(input, output, ResolveStream(stream));
}

void ReluInplace(DeviceTensor& tensor, cudaStream_t stream) {
  Relu(tensor, tensor, stream);
}

void Relu(const cv::cuda::GpuMat& input, cv::cuda::GpuMat& output, cv::cuda::Stream* stream) {
  if (input.empty()) {
    output.release();
    return;
  }
  if (input.depth() != CV_32F) {
    throw std::runtime_error("Relu(GpuMat): only CV_32F is supported");
  }

  if (output.empty() || output.size() != input.size() || output.type() != input.type()) {
    output.create(input.rows, input.cols, input.type());
  }

  const cudaStream_t cuda_stream = GetCudaStream(stream);
  const int          channels    = input.channels();
  const int          width_f     = input.cols * channels;

  // Contiguous GpuMat: reuse the highly optimized flat path (covers most pipeline mats).
  if (input.isContinuous() && output.isContinuous()) {
    const std::int64_t n =
        static_cast<std::int64_t>(input.rows) * static_cast<std::int64_t>(width_f);
    LaunchContiguousRelu(input.ptr<float>(), output.ptr<float>(), n, cuda_stream);
    return;
  }

  // Pitched path: zero-copy, no transpose, honors step.
  const dim3 block(32, 8);
  const dim3 grid((width_f + static_cast<int>(block.x) - 1) / static_cast<int>(block.x),
                  (input.rows + static_cast<int>(block.y) - 1) / static_cast<int>(block.y));
  ReluPitched2DKernel<<<grid, block, 0, cuda_stream>>>(
      input.ptr<float>(), output.ptr<float>(), input.rows, width_f, input.step, output.step);
  CheckCuda(cudaGetLastError(), "ReluPitched2DKernel launch");
}

void ReluInplace(cv::cuda::GpuMat& image, cv::cuda::Stream* stream) {
  Relu(image, image, stream);
}

}  // namespace alcedo::cuda::nn
