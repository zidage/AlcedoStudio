//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/concat.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

// One thread per output element. Decodes NCHW index and reads from a or b.
// a: [N, Ca, H, W], b: [N, Cb, H, W], out: [N, Ca+Cb, H, W] contiguous.
__global__ void ConcatChannelsKernel(const float* __restrict__ a, const float* __restrict__ b,
                                     float* __restrict__ out, int N, int Ca, int Cb, int H, int W,
                                     std::int64_t a_stride_n, std::int64_t a_stride_c,
                                     std::int64_t a_stride_h, std::int64_t a_stride_w,
                                     std::int64_t b_stride_n, std::int64_t b_stride_c,
                                     std::int64_t b_stride_h, std::int64_t b_stride_w) {
  const int Cout = Ca + Cb;
  const std::int64_t numel =
      static_cast<std::int64_t>(N) * static_cast<std::int64_t>(Cout) * static_cast<std::int64_t>(H) *
      static_cast<std::int64_t>(W);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    // Decode contiguous NCHW out index: n, c, h, w
    std::int64_t rem = linear;
    const int    w   = static_cast<int>(rem % W);
    rem /= W;
    const int h = static_cast<int>(rem % H);
    rem /= H;
    const int c = static_cast<int>(rem % Cout);
    const int n = static_cast<int>(rem / Cout);

    float value = 0.0f;
    if (c < Ca) {
      const std::int64_t a_off = static_cast<std::int64_t>(n) * a_stride_n +
                                 static_cast<std::int64_t>(c) * a_stride_c +
                                 static_cast<std::int64_t>(h) * a_stride_h +
                                 static_cast<std::int64_t>(w) * a_stride_w;
      value = a[a_off];
    } else {
      const int bc = c - Ca;
      const std::int64_t b_off = static_cast<std::int64_t>(n) * b_stride_n +
                                 static_cast<std::int64_t>(bc) * b_stride_c +
                                 static_cast<std::int64_t>(h) * b_stride_h +
                                 static_cast<std::int64_t>(w) * b_stride_w;
      value = b[b_off];
    }
    out[linear] = value;
  }
}

// Fast path when a and b are contiguous NCHW: two bulk memcpy-like channel packs
// per batch item (kernel still used so one stream sync boundary).
// Plane size = H*W; for each n, copy Ca planes then Cb planes.
__global__ void ConcatChannelsContiguousKernel(const float* __restrict__ a,
                                               const float* __restrict__ b, float* __restrict__ out,
                                               int N, int Ca, int Cb, std::int64_t plane) {
  const int Cout = Ca + Cb;
  const std::int64_t total_planes =
      static_cast<std::int64_t>(N) * static_cast<std::int64_t>(Cout);
  const std::int64_t numel = total_planes * plane;
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < numel;
       i += grid_stride) {
    const std::int64_t plane_idx = i / plane;
    const std::int64_t within    = i % plane;
    const int          n         = static_cast<int>(plane_idx / Cout);
    const int          c         = static_cast<int>(plane_idx % Cout);

    float value = 0.0f;
    if (c < Ca) {
      const std::int64_t a_base =
          (static_cast<std::int64_t>(n) * Ca + static_cast<std::int64_t>(c)) * plane;
      value = a[a_base + within];
    } else {
      const int bc = c - Ca;
      const std::int64_t b_base =
          (static_cast<std::int64_t>(n) * Cb + static_cast<std::int64_t>(bc)) * plane;
      value = b[b_base + within];
    }
    out[i] = value;
  }
}

void ValidateRank4Nchw(const DeviceTensor& t, const char* name) {
  if (t.rank != 4) {
    throw std::runtime_error(std::string("ConcatChannels: ") + name + " must be rank-4 NCHW");
  }
  if (t.data == nullptr) {
    throw std::runtime_error(std::string("ConcatChannels: ") + name + " has null data");
  }
}

}  // namespace

void ConcatChannels(const DeviceTensor& a, const DeviceTensor& b, DeviceTensor& out,
                    cudaStream_t stream) {
  ValidateRank4Nchw(a, "a");
  ValidateRank4Nchw(b, "b");
  ValidateRank4Nchw(out, "out");

  if (!out.IsContiguous()) {
    throw std::runtime_error("ConcatChannels: output must be contiguous NCHW");
  }

  const int N  = static_cast<int>(a.shape[0]);
  const int Ca = static_cast<int>(a.shape[1]);
  const int Ha = static_cast<int>(a.shape[2]);
  const int Wa = static_cast<int>(a.shape[3]);
  const int Cb = static_cast<int>(b.shape[1]);
  const int Hb = static_cast<int>(b.shape[2]);
  const int Wb = static_cast<int>(b.shape[3]);

  if (b.shape[0] != N) {
    throw std::runtime_error("ConcatChannels: batch size mismatch");
  }
  if (Ha != Hb || Wa != Wb) {
    throw std::runtime_error("ConcatChannels: spatial size mismatch");
  }
  if (out.shape[0] != N || out.shape[1] != Ca + Cb || out.shape[2] != Ha || out.shape[3] != Wa) {
    throw std::runtime_error("ConcatChannels: output shape must be [N, Ca+Cb, H, W]");
  }
  if (Ca < 0 || Cb < 0 || Ha < 0 || Wa < 0) {
    throw std::runtime_error("ConcatChannels: negative dimensions");
  }

  const std::int64_t numel = out.Numel();
  if (numel == 0) {
    return;
  }

  const cudaStream_t s = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);

  if (a.IsContiguous() && b.IsContiguous()) {
    const std::int64_t plane = static_cast<std::int64_t>(Ha) * static_cast<std::int64_t>(Wa);
    ConcatChannelsContiguousKernel<<<grid, kBlockSize, 0, s>>>(a.data, b.data, out.data, N, Ca, Cb,
                                                                plane);
    CheckCuda(cudaGetLastError(), "ConcatChannelsContiguousKernel launch");
    return;
  }

  ConcatChannelsKernel<<<grid, kBlockSize, 0, s>>>(
      a.data, b.data, out.data, N, Ca, Cb, Ha, Wa, a.strides[0], a.strides[1], a.strides[2],
      a.strides[3], b.strides[0], b.strides[1], b.strides[2], b.strides[3]);
  CheckCuda(cudaGetLastError(), "ConcatChannelsKernel launch");
}

}  // namespace alcedo::cuda::nn
