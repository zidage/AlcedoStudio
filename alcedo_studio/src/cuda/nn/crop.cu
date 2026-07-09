//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/crop.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

void ValidateRank4(const DeviceTensor& t, const char* what) {
  if (t.rank != 4) {
    throw std::runtime_error(std::string(what) + ": tensor must be rank-4 NCHW");
  }
  if (t.data == nullptr) {
    throw std::runtime_error(std::string(what) + ": null data");
  }
}

__global__ void CenterCropSpatialKernel(const float* __restrict__ in, float* __restrict__ out,
                                        int N, int C, int src_h, int src_w, int tgt_h, int tgt_w,
                                        int crop_t, int crop_l, std::int64_t in_stride_n,
                                        std::int64_t in_stride_c, std::int64_t in_stride_h,
                                        std::int64_t in_stride_w) {
  (void)src_h;
  (void)src_w;
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(C) *
                             static_cast<std::int64_t>(tgt_h) * static_cast<std::int64_t>(tgt_w);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    w   = static_cast<int>(rem % tgt_w);
    rem /= tgt_w;
    const int h = static_cast<int>(rem % tgt_h);
    rem /= tgt_h;
    const int c = static_cast<int>(rem % C);
    const int n = static_cast<int>(rem / C);

    const int src_y = h + crop_t;
    const int src_x = w + crop_l;
    const std::int64_t in_off = static_cast<std::int64_t>(n) * in_stride_n +
                                static_cast<std::int64_t>(c) * in_stride_c +
                                static_cast<std::int64_t>(src_y) * in_stride_h +
                                static_cast<std::int64_t>(src_x) * in_stride_w;
    out[linear] = in[in_off];
  }
}

}  // namespace

auto ComputeCenterCropOffsets(int src_h, int src_w, int tgt_h, int tgt_w) -> CenterCropOffsets {
  if (src_h < 0 || src_w < 0 || tgt_h < 0 || tgt_w < 0) {
    throw std::runtime_error("ComputeCenterCropOffsets: negative size");
  }
  if (tgt_h > src_h || tgt_w > src_w) {
    throw std::runtime_error("ComputeCenterCropOffsets: target exceeds source spatial size");
  }
  // demosaicnet / PyTorch: integer floor division on each side, remainder on bottom/right.
  const int crop_h = src_h - tgt_h;
  const int crop_t = crop_h / 2;
  const int crop_w = src_w - tgt_w;
  const int crop_l = crop_w / 2;

  CenterCropOffsets o;
  o.top   = crop_t;
  o.left  = crop_l;
  o.src_h = src_h;
  o.src_w = src_w;
  o.tgt_h = tgt_h;
  o.tgt_w = tgt_w;
  return o;
}

auto CenterCropSpatialView(const DeviceTensor& input, int target_h, int target_w) -> DeviceTensor {
  ValidateRank4(input, "CenterCropSpatialView");
  const int src_h = static_cast<int>(input.shape[2]);
  const int src_w = static_cast<int>(input.shape[3]);
  const auto offs = ComputeCenterCropOffsets(src_h, src_w, target_h, target_w);

  DeviceTensor view;
  view.data = input.data + static_cast<std::int64_t>(offs.top) * input.strides[2] +
              static_cast<std::int64_t>(offs.left) * input.strides[3];
  view.rank       = 4;
  view.shape[0]   = input.shape[0];
  view.shape[1]   = input.shape[1];
  view.shape[2]   = target_h;
  view.shape[3]   = target_w;
  view.strides[0] = input.strides[0];
  view.strides[1] = input.strides[1];
  view.strides[2] = input.strides[2];
  view.strides[3] = input.strides[3];
  return view;
}

void CenterCropSpatial(const DeviceTensor& input, DeviceTensor& out, int target_h, int target_w,
                       cudaStream_t stream) {
  ValidateRank4(input, "CenterCropSpatial");
  if (out.data == nullptr || out.rank != 4) {
    throw std::runtime_error("CenterCropSpatial: out must be rank-4 with valid data");
  }
  if (!out.IsContiguous()) {
    throw std::runtime_error("CenterCropSpatial: out must be contiguous");
  }

  const int N     = static_cast<int>(input.shape[0]);
  const int C     = static_cast<int>(input.shape[1]);
  const int src_h = static_cast<int>(input.shape[2]);
  const int src_w = static_cast<int>(input.shape[3]);
  const auto offs = ComputeCenterCropOffsets(src_h, src_w, target_h, target_w);

  if (out.shape[0] != N || out.shape[1] != C || out.shape[2] != target_h ||
      out.shape[3] != target_w) {
    throw std::runtime_error("CenterCropSpatial: out shape must be [N, C, target_h, target_w]");
  }

  const std::int64_t numel = out.Numel();
  if (numel == 0) {
    return;
  }

  // Identity crop: optional fast path.
  if (target_h == src_h && target_w == src_w && input.IsContiguous()) {
    const std::size_t bytes = static_cast<std::size_t>(numel) * sizeof(float);
    if (stream == nullptr) {
      CheckCuda(cudaMemcpy(out.data, input.data, bytes, cudaMemcpyDeviceToDevice),
                "CenterCropSpatial identity D2D");
    } else {
      CheckCuda(cudaMemcpyAsync(out.data, input.data, bytes, cudaMemcpyDeviceToDevice, stream),
                "CenterCropSpatial identity D2D async");
    }
    return;
  }

  const cudaStream_t s    = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);
  CenterCropSpatialKernel<<<grid, kBlockSize, 0, s>>>(
      input.data, out.data, N, C, src_h, src_w, target_h, target_w, offs.top, offs.left,
      input.strides[0], input.strides[1], input.strides[2], input.strides[3]);
  CheckCuda(cudaGetLastError(), "CenterCropSpatialKernel launch");
}

void CenterCropLike(const DeviceTensor& input, const DeviceTensor& spatial_ref, DeviceTensor& out,
                    cudaStream_t stream) {
  ValidateRank4(spatial_ref, "CenterCropLike spatial_ref");
  CenterCropSpatial(input, out, static_cast<int>(spatial_ref.shape[2]),
                    static_cast<int>(spatial_ref.shape[3]), stream);
}

}  // namespace alcedo::cuda::nn
