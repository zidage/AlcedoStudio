//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/layout.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <opencv2/core/cuda_stream_accessor.hpp>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  if (stream == nullptr) {
    return nullptr;
  }
  return cv::cuda::StreamAccessor::getStream(*stream);
}

// Pack HWC → NCHW: out[n,c,h,w] = in[n,h,w,c]
__global__ void PackHwcToNchwKernel(const float* __restrict__ hwc, float* __restrict__ nchw, int N,
                                    int C, int H, int W, std::int64_t hwc_stride_n,
                                    std::int64_t hwc_stride_h, std::int64_t hwc_stride_w,
                                    std::int64_t hwc_stride_c) {
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(C) *
                             static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    // Decode contiguous NCHW index
    std::int64_t rem = linear;
    const int    w   = static_cast<int>(rem % W);
    rem /= W;
    const int h = static_cast<int>(rem % H);
    rem /= H;
    const int c = static_cast<int>(rem % C);
    const int n = static_cast<int>(rem / C);

    const std::int64_t in_off = static_cast<std::int64_t>(n) * hwc_stride_n +
                                static_cast<std::int64_t>(h) * hwc_stride_h +
                                static_cast<std::int64_t>(w) * hwc_stride_w +
                                static_cast<std::int64_t>(c) * hwc_stride_c;
    nchw[linear] = hwc[in_off];
  }
}

// Unpack NCHW → HWC: out[n,h,w,c] = in[n,c,h,w]
__global__ void UnpackNchwToHwcKernel(const float* __restrict__ nchw, float* __restrict__ hwc, int N,
                                      int C, int H, int W, std::int64_t nchw_stride_n,
                                      std::int64_t nchw_stride_c, std::int64_t nchw_stride_h,
                                      std::int64_t nchw_stride_w) {
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(H) *
                             static_cast<std::int64_t>(W) * static_cast<std::int64_t>(C);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    // Decode contiguous HWC index [N,H,W,C]
    std::int64_t rem = linear;
    const int    c   = static_cast<int>(rem % C);
    rem /= C;
    const int w = static_cast<int>(rem % W);
    rem /= W;
    const int h = static_cast<int>(rem % H);
    const int n = static_cast<int>(rem / H);

    const std::int64_t in_off = static_cast<std::int64_t>(n) * nchw_stride_n +
                                static_cast<std::int64_t>(c) * nchw_stride_c +
                                static_cast<std::int64_t>(h) * nchw_stride_h +
                                static_cast<std::int64_t>(w) * nchw_stride_w;
    hwc[linear] = nchw[in_off];
  }
}

// Pitched GpuMat HWC pack: row step in elements (floats), channels packed in-pixel.
__global__ void PackGpuMatHwcToNchwKernel(const float* __restrict__ hwc, float* __restrict__ nchw,
                                          int C, int H, int W, std::size_t row_step_bytes) {
  const std::int64_t numel =
      static_cast<std::int64_t>(C) * static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    w   = static_cast<int>(rem % W);
    rem /= W;
    const int h = static_cast<int>(rem % H);
    const int c = static_cast<int>(rem / H);

    const auto* row = reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(hwc) + static_cast<std::size_t>(h) * row_step_bytes);
    nchw[linear] = row[static_cast<std::int64_t>(w) * C + c];
  }
}

__global__ void UnpackNchwToGpuMatHwcKernel(const float* __restrict__ nchw, float* __restrict__ hwc,
                                            int C, int H, int W, std::size_t row_step_bytes) {
  const std::int64_t numel =
      static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W) * static_cast<std::int64_t>(C);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  // Iterate HWC linear for coalesced writes when continuous; still correct when pitched.
  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    c   = static_cast<int>(rem % C);
    rem /= C;
    const int w = static_cast<int>(rem % W);
    const int h = static_cast<int>(rem / W);

    // NCHW index for N=1: c * H * W + h * W + w
    const std::int64_t nchw_off = static_cast<std::int64_t>(c) * H * W +
                                  static_cast<std::int64_t>(h) * W + static_cast<std::int64_t>(w);
    auto* row = reinterpret_cast<float*>(reinterpret_cast<char*>(hwc) +
                                         static_cast<std::size_t>(h) * row_step_bytes);
    row[static_cast<std::int64_t>(w) * C + c] = nchw[nchw_off];
  }
}

struct HwcLayout {
  int          N = 1;
  int          H = 0;
  int          W = 0;
  int          C = 0;
  std::int64_t stride_n = 0;
  std::int64_t stride_h = 0;
  std::int64_t stride_w = 0;
  std::int64_t stride_c = 0;
  const float* data     = nullptr;
};

struct NchwLayout {
  int          N = 1;
  int          C = 0;
  int          H = 0;
  int          W = 0;
  std::int64_t stride_n = 0;
  std::int64_t stride_c = 0;
  std::int64_t stride_h = 0;
  std::int64_t stride_w = 0;
  float*       data     = nullptr;
  const float* cdata    = nullptr;
};

auto ParseHwc(const DeviceTensor& t) -> HwcLayout {
  HwcLayout L;
  L.data = t.data;
  if (t.rank == 3) {
    // [H, W, C]
    L.N        = 1;
    L.H        = static_cast<int>(t.shape[0]);
    L.W        = static_cast<int>(t.shape[1]);
    L.C        = static_cast<int>(t.shape[2]);
    L.stride_n = 0;
    L.stride_h = t.strides[0];
    L.stride_w = t.strides[1];
    L.stride_c = t.strides[2];
  } else if (t.rank == 4) {
    // [N, H, W, C]
    L.N        = static_cast<int>(t.shape[0]);
    L.H        = static_cast<int>(t.shape[1]);
    L.W        = static_cast<int>(t.shape[2]);
    L.C        = static_cast<int>(t.shape[3]);
    L.stride_n = t.strides[0];
    L.stride_h = t.strides[1];
    L.stride_w = t.strides[2];
    L.stride_c = t.strides[3];
  } else {
    throw std::runtime_error("PackHwcToNchw: HWC tensor must be rank-3 [H,W,C] or rank-4 [N,H,W,C]");
  }
  if (L.data == nullptr) {
    throw std::runtime_error("PackHwcToNchw: null HWC data");
  }
  return L;
}

auto ParseNchw(const DeviceTensor& t, bool require_writable) -> NchwLayout {
  NchwLayout L;
  if (require_writable) {
    L.data = t.data;
  } else {
    L.cdata = t.data;
  }
  if (t.rank == 3) {
    // [C, H, W]
    L.N        = 1;
    L.C        = static_cast<int>(t.shape[0]);
    L.H        = static_cast<int>(t.shape[1]);
    L.W        = static_cast<int>(t.shape[2]);
    L.stride_n = 0;
    L.stride_c = t.strides[0];
    L.stride_h = t.strides[1];
    L.stride_w = t.strides[2];
  } else if (t.rank == 4) {
    L.N        = static_cast<int>(t.shape[0]);
    L.C        = static_cast<int>(t.shape[1]);
    L.H        = static_cast<int>(t.shape[2]);
    L.W        = static_cast<int>(t.shape[3]);
    L.stride_n = t.strides[0];
    L.stride_c = t.strides[1];
    L.stride_h = t.strides[2];
    L.stride_w = t.strides[3];
  } else {
    throw std::runtime_error(
        "layout: NCHW tensor must be rank-3 [C,H,W] or rank-4 [N,C,H,W]");
  }
  if (t.data == nullptr) {
    throw std::runtime_error("layout: null NCHW data");
  }
  return L;
}

void ValidateNchwOutMatchesHwc(const HwcLayout& hwc, const DeviceTensor& nchw) {
  if (!nchw.IsContiguous()) {
    throw std::runtime_error("PackHwcToNchw: NCHW output must be contiguous");
  }
  const auto n = ParseNchw(nchw, true);
  if (n.N != hwc.N || n.C != hwc.C || n.H != hwc.H || n.W != hwc.W) {
    throw std::runtime_error("PackHwcToNchw: NCHW shape must be [N,C,H,W] matching HWC");
  }
}

void ValidateHwcOutMatchesNchw(const NchwLayout& nchw, const DeviceTensor& hwc) {
  if (!hwc.IsContiguous()) {
    throw std::runtime_error("UnpackNchwToHwc: HWC output must be contiguous");
  }
  const auto h = ParseHwc(hwc);
  if (h.N != nchw.N || h.C != nchw.C || h.H != nchw.H || h.W != nchw.W) {
    throw std::runtime_error("UnpackNchwToHwc: HWC shape must match NCHW spatial/channels");
  }
}

}  // namespace

void PackHwcToNchw(const DeviceTensor& hwc, DeviceTensor& nchw, cudaStream_t stream) {
  const HwcLayout in = ParseHwc(hwc);
  ValidateNchwOutMatchesHwc(in, nchw);

  const std::int64_t numel = nchw.Numel();
  if (numel == 0) {
    return;
  }

  const cudaStream_t s    = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);
  PackHwcToNchwKernel<<<grid, kBlockSize, 0, s>>>(in.data, nchw.data, in.N, in.C, in.H, in.W,
                                                    in.stride_n, in.stride_h, in.stride_w,
                                                    in.stride_c);
  CheckCuda(cudaGetLastError(), "PackHwcToNchwKernel launch");
}

void UnpackNchwToHwc(const DeviceTensor& nchw, DeviceTensor& hwc, cudaStream_t stream) {
  const NchwLayout in = ParseNchw(nchw, false);
  ValidateHwcOutMatchesNchw(in, hwc);

  const std::int64_t numel = hwc.Numel();
  if (numel == 0) {
    return;
  }

  const cudaStream_t s    = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);
  UnpackNchwToHwcKernel<<<grid, kBlockSize, 0, s>>>(in.cdata, hwc.data, in.N, in.C, in.H, in.W,
                                                      in.stride_n, in.stride_c, in.stride_h,
                                                      in.stride_w);
  CheckCuda(cudaGetLastError(), "UnpackNchwToHwcKernel launch");
}

void PackHwcToNchw(const cv::cuda::GpuMat& hwc, DeviceTensor& nchw, cudaStream_t stream) {
  if (hwc.empty()) {
    if (nchw.Numel() != 0) {
      throw std::runtime_error("PackHwcToNchw(GpuMat): empty input but non-empty NCHW");
    }
    return;
  }
  if (hwc.depth() != CV_32F) {
    throw std::runtime_error("PackHwcToNchw(GpuMat): only CV_32F is supported");
  }
  if (nchw.data == nullptr || !nchw.IsContiguous()) {
    throw std::runtime_error("PackHwcToNchw(GpuMat): NCHW must be contiguous with valid data");
  }

  const int C = hwc.channels();
  const int H = hwc.rows;
  const int W = hwc.cols;

  // Accept [1,C,H,W] or [C,H,W]
  bool shape_ok = false;
  if (nchw.rank == 4 && nchw.shape[0] == 1 && nchw.shape[1] == C && nchw.shape[2] == H &&
      nchw.shape[3] == W) {
    shape_ok = true;
  }
  if (nchw.rank == 3 && nchw.shape[0] == C && nchw.shape[1] == H && nchw.shape[2] == W) {
    shape_ok = true;
  }
  if (!shape_ok) {
    throw std::runtime_error("PackHwcToNchw(GpuMat): NCHW shape must be [1,C,H,W] or [C,H,W]");
  }

  const std::int64_t numel =
      static_cast<std::int64_t>(C) * static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
  if (numel == 0) {
    return;
  }

  const cudaStream_t s    = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);
  PackGpuMatHwcToNchwKernel<<<grid, kBlockSize, 0, s>>>(hwc.ptr<float>(), nchw.data, C, H, W,
                                                         hwc.step);
  CheckCuda(cudaGetLastError(), "PackGpuMatHwcToNchwKernel launch");
}

void PackHwcToNchw(const cv::cuda::GpuMat& hwc, DeviceTensor& nchw, cv::cuda::Stream* stream) {
  PackHwcToNchw(hwc, nchw, GetCudaStream(stream));
}

void UnpackNchwToHwc(const DeviceTensor& nchw, cv::cuda::GpuMat& hwc, cudaStream_t stream) {
  const NchwLayout in = ParseNchw(nchw, false);
  if (in.N != 1) {
    throw std::runtime_error("UnpackNchwToHwc(GpuMat): only N=1 is supported");
  }
  if (in.C <= 0 || in.C > CV_CN_MAX) {
    throw std::runtime_error("UnpackNchwToHwc(GpuMat): invalid channel count");
  }

  const int type = CV_MAKE_TYPE(CV_32F, in.C);
  if (hwc.empty() || hwc.rows != in.H || hwc.cols != in.W || hwc.type() != type) {
    hwc.create(in.H, in.W, type);
  }

  const std::int64_t numel =
      static_cast<std::int64_t>(in.C) * static_cast<std::int64_t>(in.H) *
      static_cast<std::int64_t>(in.W);
  if (numel == 0) {
    return;
  }

  // Prefer reading as contiguous NCHW [C,H,W] / [1,C,H,W].
  if (!nchw.IsContiguous()) {
    throw std::runtime_error("UnpackNchwToHwc(GpuMat): NCHW input must be contiguous");
  }

  const cudaStream_t s    = ResolveStream(stream);
  const int          grid = ChooseGridSize(numel, kBlockSize);
  UnpackNchwToGpuMatHwcKernel<<<grid, kBlockSize, 0, s>>>(nchw.data, hwc.ptr<float>(), in.C, in.H,
                                                           in.W, hwc.step);
  CheckCuda(cudaGetLastError(), "UnpackNchwToGpuMatHwcKernel launch");
}

void UnpackNchwToHwc(const DeviceTensor& nchw, cv::cuda::GpuMat& hwc, cv::cuda::Stream* stream) {
  UnpackNchwToHwc(nchw, hwc, GetCudaStream(stream));
}

}  // namespace alcedo::cuda::nn
