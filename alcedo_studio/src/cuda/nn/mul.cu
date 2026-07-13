//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/mul.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

__global__ void MulScalarKernel(const float* __restrict__ a, const float* __restrict__ b,
                                float* __restrict__ out, std::int64_t n) {
  const std::int64_t stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    out[i] = a[i] * b[i];
  }
}

__global__ void MulFloat4Kernel(const float4* __restrict__ a, const float4* __restrict__ b,
                                float4* __restrict__ out, std::int64_t n4) {
  const std::int64_t stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n4;
       i += stride) {
    const float4 va = a[i];
    const float4 vb = b[i];
    float4       vo;
    vo.x   = va.x * vb.x;
    vo.y   = va.y * vb.y;
    vo.z   = va.z * vb.z;
    vo.w   = va.w * vb.w;
    out[i] = vo;
  }
}

struct StridedMulMeta {
  int          rank = 0;
  std::int64_t shape[kMaxTensorRank]       = {};
  std::int64_t a_strides[kMaxTensorRank]   = {};
  std::int64_t b_strides[kMaxTensorRank]   = {};
  std::int64_t out_strides[kMaxTensorRank] = {};
};

__global__ void MulStridedKernel(const float* __restrict__ a, const float* __restrict__ b,
                                 float* __restrict__ out, std::int64_t numel,
                                 StridedMulMeta meta) {
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);
  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t remaining  = linear;
    std::int64_t a_offset   = 0;
    std::int64_t b_offset   = 0;
    std::int64_t out_offset = 0;
    for (int d = meta.rank - 1; d >= 0; --d) {
      const std::int64_t coord = remaining % meta.shape[d];
      remaining /= meta.shape[d];
      a_offset += coord * meta.a_strides[d];
      b_offset += coord * meta.b_strides[d];
      out_offset += coord * meta.out_strides[d];
    }
    out[out_offset] = a[a_offset] * b[b_offset];
  }
}

auto IsDevicePointerAligned(const void* ptr, std::size_t alignment) -> bool {
  return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
}

void LaunchContiguousMul(const float* a, const float* b, float* out, std::int64_t n,
                         cudaStream_t stream) {
  if (n <= 0) {
    return;
  }

  const bool can_vectorize = IsDevicePointerAligned(a, 16) && IsDevicePointerAligned(b, 16) &&
                             IsDevicePointerAligned(out, 16);
  if (can_vectorize && n >= 4) {
    const std::int64_t n4    = n / 4;
    const int          grid4 = ChooseGridSize(n4, kBlockSize);
    MulFloat4Kernel<<<grid4, kBlockSize, 0, stream>>>(reinterpret_cast<const float4*>(a),
                                                      reinterpret_cast<const float4*>(b),
                                                      reinterpret_cast<float4*>(out), n4);
    CheckCuda(cudaGetLastError(), "MulFloat4Kernel launch");

    const std::int64_t tail = n - n4 * 4;
    if (tail > 0) {
      const std::int64_t offset    = n4 * 4;
      const int          grid_tail = ChooseGridSize(tail, kBlockSize);
      MulScalarKernel<<<grid_tail, kBlockSize, 0, stream>>>(a + offset, b + offset, out + offset,
                                                            tail);
      CheckCuda(cudaGetLastError(), "MulScalarKernel tail launch");
    }
    return;
  }

  const int grid = ChooseGridSize(n, kBlockSize);
  MulScalarKernel<<<grid, kBlockSize, 0, stream>>>(a, b, out, n);
  CheckCuda(cudaGetLastError(), "MulScalarKernel launch");
}

void LaunchStridedMul(const DeviceTensor& a, const DeviceTensor& b, DeviceTensor& out,
                      cudaStream_t stream) {
  const std::int64_t numel = a.Numel();
  if (numel <= 0) {
    return;
  }

  StridedMulMeta meta;
  meta.rank = a.rank;
  for (int i = 0; i < a.rank; ++i) {
    meta.shape[i]       = a.shape[i];
    meta.a_strides[i]   = a.strides[i];
    meta.b_strides[i]   = b.strides[i];
    meta.out_strides[i] = out.strides[i];
  }

  const int grid = ChooseGridSize(numel, kBlockSize);
  MulStridedKernel<<<grid, kBlockSize, 0, stream>>>(a.data, b.data, out.data, numel, meta);
  CheckCuda(cudaGetLastError(), "MulStridedKernel launch");
}

void ValidateSameShape(const DeviceTensor& x, const DeviceTensor& y, const char* what) {
  if (x.rank != y.rank) {
    throw std::runtime_error(std::string(what) + ": rank mismatch");
  }
  for (int i = 0; i < x.rank; ++i) {
    if (x.shape[i] != y.shape[i]) {
      throw std::runtime_error(std::string(what) + ": shape mismatch");
    }
  }
}

}  // namespace

void Mul(const float* a, const float* b, float* out, std::int64_t n, cudaStream_t stream) {
  if (n < 0) {
    throw std::runtime_error("Mul: negative element count");
  }
  if (n == 0) {
    return;
  }
  if (a == nullptr || b == nullptr || out == nullptr) {
    throw std::runtime_error("Mul: null device pointer");
  }
  LaunchContiguousMul(a, b, out, n, ResolveStream(stream));
}

void MulInplace(float* a, const float* b, std::int64_t n, cudaStream_t stream) {
  Mul(a, b, a, n, stream);
}

void Mul(const DeviceTensor& a, const DeviceTensor& b, DeviceTensor& out, cudaStream_t stream) {
  if (a.data == nullptr || b.data == nullptr || out.data == nullptr) {
    throw std::runtime_error("Mul: null DeviceTensor data");
  }
  ValidateSameShape(a, b, "Mul");
  ValidateSameShape(a, out, "Mul");

  const std::int64_t numel = a.Numel();
  if (numel == 0) {
    return;
  }

  if (a.IsContiguous() && b.IsContiguous() && out.IsContiguous()) {
    LaunchContiguousMul(a.data, b.data, out.data, numel, ResolveStream(stream));
    return;
  }

  LaunchStridedMul(a, b, out, ResolveStream(stream));
}

void MulInplace(DeviceTensor& a, const DeviceTensor& b, cudaStream_t stream) {
  Mul(a, b, a, stream);
}

}  // namespace alcedo::cuda::nn
