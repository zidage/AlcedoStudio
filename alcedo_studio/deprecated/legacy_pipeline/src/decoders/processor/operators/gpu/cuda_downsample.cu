//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/gpu/cuda_downsample.hpp"

#include <cuda_runtime.h>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>

#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"

namespace alcedo {
namespace CUDA {
namespace {

struct Bayer2xOffsets {
  std::array<int, 4> row_delta = {};
  std::array<int, 4> col_delta = {};
};

__host__ __device__ inline auto BayerCellIndexDevice(const int y, const int x) -> int {
  return ((y & 1) << 1) | (x & 1);
}

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  if (stream == nullptr) {
    return nullptr;
  }
  return cv::cuda::StreamAccessor::getStream(*stream);
}

void MaybeSync(cudaStream_t stream) {
  if (stream == nullptr) {
    CUDA_CHECK(cudaDeviceSynchronize());
  }
}

auto BuildBayer2xOffsets(const BayerPattern2x2& pattern) -> Bayer2xOffsets {
  Bayer2xOffsets offsets;

  std::array<int, 4> src_index_for_dst = {-1, -1, -1, -1};
  for (int dst_index = 0; dst_index < 4; ++dst_index) {
    const int expected_color = pattern.raw_fc[dst_index];
    int       src_index      = -1;
    for (int i = 0; i < 4; ++i) {
      if (pattern.raw_fc[i] != expected_color) {
        continue;
      }
      if (src_index >= 0) {
        throw std::runtime_error("CUDA::DownsampleRaw2x: duplicated Bayer color index in pattern");
      }
      src_index = i;
    }
    if (src_index < 0) {
      throw std::runtime_error("CUDA::DownsampleRaw2x: invalid Bayer pattern");
    }
    src_index_for_dst[dst_index] = src_index;
  }

  for (int i = 0; i < 4; ++i) {
    offsets.row_delta[i] = src_index_for_dst[i] >> 1;
    offsets.col_delta[i] = src_index_for_dst[i] & 1;
  }
  return offsets;
}

void UpdateXTransPatternFor2xDownsample(XTransPattern6x6& pattern) {
  XTransPattern6x6 sampled_pattern = {};
  for (int row = 0; row < 6; ++row) {
    for (int col = 0; col < 6; ++col) {
      const int index               = XTransCellIndex(row, col);
      sampled_pattern.raw_fc[index] = RawColorAt(pattern, 2 * row, 2 * col);
      sampled_pattern.rgb_fc[index] = FoldRawColorToRgb(sampled_pattern.raw_fc[index]);
    }
  }
  pattern = sampled_pattern;
}

template <typename T>
__global__ void DownsampleBayer2xKernel(cv::cuda::PtrStepSz<T> src, cv::cuda::PtrStepSz<T> dst,
                                        const int row_delta0, const int col_delta0,
                                        const int row_delta1, const int col_delta1,
                                        const int row_delta2, const int col_delta2,
                                        const int row_delta3, const int col_delta3) {
  const int x_pair = blockIdx.x * blockDim.x + threadIdx.x;
  const int y      = blockIdx.y * blockDim.y + threadIdx.y;
  if (y >= dst.rows) {
    return;
  }

  const int x0 = x_pair << 1;
  if (x0 >= dst.cols) {
    return;
  }

  const int dst_even_idx = BayerCellIndexDevice(y, 0);
  const int dst_odd_idx  = BayerCellIndexDevice(y, 1);

  const int row_delta_even = dst_even_idx == 0 ? row_delta0 : row_delta2;
  const int col_delta_even = dst_even_idx == 0 ? col_delta0 : col_delta2;
  const int row_delta_odd  = dst_odd_idx == 1 ? row_delta1 : row_delta3;
  const int col_delta_odd  = dst_odd_idx == 1 ? col_delta1 : col_delta3;

  const int src_y_even = (y << 1) + row_delta_even;
  const int src_y_odd  = (y << 1) + row_delta_odd;

  T*       dst_row     = dst.ptr(y);
  const T* src_even    = src.ptr(src_y_even);
  const T* src_odd     = src.ptr(src_y_odd);

  dst_row[x0]          = src_even[(x0 << 1) + col_delta_even];
  const int x1         = x0 + 1;
  if (x1 < dst.cols) {
    dst_row[x1] = src_odd[(x1 << 1) + col_delta_odd];
  }
}

template <typename T>
__global__ void DownsampleXTrans2xKernel(cv::cuda::PtrStepSz<T> src, cv::cuda::PtrStepSz<T> dst) {
  const int x_pair = blockIdx.x * blockDim.x + threadIdx.x;
  const int y      = blockIdx.y * blockDim.y + threadIdx.y;
  if (y >= dst.rows) {
    return;
  }

  const int x0 = x_pair << 1;
  if (x0 >= dst.cols) {
    return;
  }

  T*       dst_row  = dst.ptr(y);
  const T* src_row0 = src.ptr(y << 1);

  dst_row[x0]       = src_row0[x0 << 1];
  const int x1      = x0 + 1;
  if (x1 < dst.cols) {
    dst_row[x1] = src_row0[x1 << 1];
  }
}

template <typename T>
void DownsampleBayer2xImpl(cv::cuda::GpuMat& image, const BayerPattern2x2& pattern,
                           const cudaStream_t stream) {
  const int out_rows = image.rows / 2;
  const int out_cols = image.cols / 2;

  cv::cuda::GpuMat out(out_rows, out_cols, image.type());
  if (out.empty()) {
    image.release();
    return;
  }

  const Bayer2xOffsets offsets = BuildBayer2xOffsets(pattern);

  const dim3 block(16, 16);
  const dim3 grid((out_cols + (block.x * 2 - 1)) / (block.x * 2),
                  (out_rows + block.y - 1) / block.y);
  DownsampleBayer2xKernel<T><<<grid, block, 0, stream>>>(
      image, out, offsets.row_delta[0], offsets.col_delta[0], offsets.row_delta[1],
      offsets.col_delta[1], offsets.row_delta[2], offsets.col_delta[2], offsets.row_delta[3],
      offsets.col_delta[3]);
  CUDA_CHECK(cudaGetLastError());
  image = out;
}

template <typename T>
void DownsampleXTrans2xImpl(cv::cuda::GpuMat& image, XTransPattern6x6& pattern,
                            const cudaStream_t stream) {
  const int out_rows = image.rows / 2;
  const int out_cols = image.cols / 2;

  cv::cuda::GpuMat out(out_rows, out_cols, image.type());
  if (out.empty()) {
    image.release();
    return;
  }

  const dim3 block(16, 16);
  const dim3 grid((out_cols + (block.x * 2 - 1)) / (block.x * 2),
                  (out_rows + block.y - 1) / block.y);
  DownsampleXTrans2xKernel<T><<<grid, block, 0, stream>>>(image, out);
  CUDA_CHECK(cudaGetLastError());

  image = out;
  UpdateXTransPatternFor2xDownsample(pattern);
}

void DownsampleRaw2xImpl(cv::cuda::GpuMat& image, RawCfaPattern& pattern, cudaStream_t stream) {
  if (image.empty()) {
    return;
  }
  if (image.rows < 2 || image.cols < 2) {
    image.release();
    return;
  }

  const int type = image.type();
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    switch (type) {
      case CV_16UC1:
        DownsampleXTrans2xImpl<uint16_t>(image, pattern.xtrans_pattern, stream);
        return;
      case CV_32FC1:
        DownsampleXTrans2xImpl<float>(image, pattern.xtrans_pattern, stream);
        return;
      default:
        throw std::runtime_error("CUDA::DownsampleRaw2x: unsupported X-Trans type");
    }
  }

  switch (type) {
    case CV_16UC1:
      DownsampleBayer2xImpl<uint16_t>(image, pattern.bayer_pattern, stream);
      return;
    case CV_32FC1:
      DownsampleBayer2xImpl<float>(image, pattern.bayer_pattern, stream);
      return;
    default:
      throw std::runtime_error("CUDA::DownsampleRaw2x: unsupported Bayer type");
  }
}

}  // namespace

void DownsampleRaw2x(cv::cuda::GpuMat& image, RawCfaPattern& pattern, cv::cuda::Stream* stream) {
  const cudaStream_t cuda_stream = GetCudaStream(stream);
  DownsampleRaw2xImpl(image, pattern, cuda_stream);
  MaybeSync(cuda_stream);
}

void DownsampleRaw(cv::cuda::GpuMat& image, RawCfaPattern& pattern, const int passes,
                   cv::cuda::Stream* stream) {
  if (passes <= 0 || image.empty()) {
    return;
  }

  const cudaStream_t cuda_stream = GetCudaStream(stream);
  for (int pass = 0; pass < passes && !image.empty(); ++pass) {
    DownsampleRaw2xImpl(image, pattern, cuda_stream);
  }
  MaybeSync(cuda_stream);
}

}  // namespace CUDA
}  // namespace alcedo
