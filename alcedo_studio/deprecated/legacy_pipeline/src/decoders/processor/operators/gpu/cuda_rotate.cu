//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/operators/gpu/cuda_rotate.hpp"

#include <cuda_runtime.h>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>

#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"

namespace alcedo {
namespace CUDA {
namespace {

constexpr int kTileDim   = 32;
constexpr int kBlockRows = 8;

template <typename T>
__global__ void Rotate90CWKernel(cv::cuda::PtrStepSz<T> src, cv::cuda::PtrStepSz<T> dst) {
  __shared__ T tile[kTileDim][kTileDim + 1];

  const int src_col = blockIdx.x * kTileDim + threadIdx.x;
  const int src_row = blockIdx.y * kTileDim + threadIdx.y;

  #pragma unroll
  for (int i = 0; i < kTileDim; i += kBlockRows) {
    const int row = src_row + i;
    if (src_col < src.cols && row < src.rows) {
      tile[threadIdx.y + i][threadIdx.x] = src(row, src_col);
    }
  }

  __syncthreads();

  const int dst_col = src.rows - 1 - (blockIdx.y * kTileDim + threadIdx.x);
  const int dst_row = blockIdx.x * kTileDim + threadIdx.y;

  #pragma unroll
  for (int i = 0; i < kTileDim; i += kBlockRows) {
    const int row = dst_row + i;
    if (row < dst.rows && dst_col >= 0 && dst_col < dst.cols) {
      dst(row, dst_col) = tile[threadIdx.x][threadIdx.y + i];
    }
  }
}

template <typename T>
__global__ void Rotate90CCWKernel(cv::cuda::PtrStepSz<T> src, cv::cuda::PtrStepSz<T> dst) {
  __shared__ T tile[kTileDim][kTileDim + 1];

  const int src_col = blockIdx.x * kTileDim + threadIdx.x;
  const int src_row = blockIdx.y * kTileDim + threadIdx.y;

  #pragma unroll
  for (int i = 0; i < kTileDim; i += kBlockRows) {
    const int row = src_row + i;
    if (src_col < src.cols && row < src.rows) {
      tile[threadIdx.y + i][threadIdx.x] = src(row, src_col);
    }
  }

  __syncthreads();

  const int dst_col = blockIdx.y * kTileDim + threadIdx.x;
  const int dst_row = dst.rows - 1 - (blockIdx.x * kTileDim + threadIdx.y);

  #pragma unroll
  for (int i = 0; i < kTileDim; i += kBlockRows) {
    const int row = dst_row - i;
    if (row >= 0 && row < dst.rows && dst_col < dst.cols) {
      dst(row, dst_col) = tile[threadIdx.x][threadIdx.y + i];
    }
  }
}

template <typename T>
__global__ void Rotate180Kernel(cv::cuda::PtrStepSz<T> src, cv::cuda::PtrStepSz<T> dst) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;

  if (x >= dst.cols || y >= dst.rows) return;

  const int src_row = src.rows - 1 - y;
  const int src_col = src.cols - 1 - x;
  dst(y, x)         = src(src_row, src_col);
}

template <typename T>
static void Rotate90CW_Impl(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                            cv::cuda::Stream& stream) {
  dst.create(src.cols, src.rows, src.type());
  cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);

  const dim3 block(kTileDim, kBlockRows);
  const dim3 grid((src.cols + kTileDim - 1) / kTileDim, (src.rows + kTileDim - 1) / kTileDim);
  Rotate90CWKernel<T><<<grid, block, 0, cuda_stream>>>(src, dst);
  CUDA_CHECK(cudaGetLastError());
}

template <typename T>
static void Rotate90CCW_Impl(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                             cv::cuda::Stream& stream) {
  dst.create(src.cols, src.rows, src.type());
  cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);

  const dim3 block(kTileDim, kBlockRows);
  const dim3 grid((src.cols + kTileDim - 1) / kTileDim, (src.rows + kTileDim - 1) / kTileDim);
  Rotate90CCWKernel<T><<<grid, block, 0, cuda_stream>>>(src, dst);
  CUDA_CHECK(cudaGetLastError());
}

template <typename T>
static void Rotate180_Impl(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                           cv::cuda::Stream& stream) {
  dst.create(src.rows, src.cols, src.type());
  cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);

  const dim3 block(32, 8);
  const dim3 grid((dst.cols + block.x - 1) / block.x, (dst.rows + block.y - 1) / block.y);
  Rotate180Kernel<T><<<grid, block, 0, cuda_stream>>>(src, dst);
  CUDA_CHECK(cudaGetLastError());
}

}  // namespace

void Rotate180(cv::cuda::GpuMat& img, cv::cuda::Stream* stream) {
  if (img.empty()) return;
  cv::cuda::Stream local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;
  cv::cuda::GpuMat out;

  switch (img.type()) {
    case CV_32FC4:
      Rotate180_Impl<float4>(img, out, active_stream);
      break;
    case CV_32FC3:
      Rotate180_Impl<float3>(img, out, active_stream);
      break;
    case CV_32FC1:
      Rotate180_Impl<float>(img, out, active_stream);
      break;
    default:
      CV_Error(cv::Error::StsUnsupportedFormat, "CUDA::Rotate180: unsupported type");
  }

  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
  img = out;
}

void Rotate90CW(cv::cuda::GpuMat& img, cv::cuda::Stream* stream) {
  if (img.empty()) return;
  cv::cuda::Stream local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;
  cv::cuda::GpuMat out;

  switch (img.type()) {
    case CV_32FC4:
      Rotate90CW_Impl<float4>(img, out, active_stream);
      break;
    case CV_32FC3:
      Rotate90CW_Impl<float3>(img, out, active_stream);
      break;
    case CV_32FC1:
      Rotate90CW_Impl<float>(img, out, active_stream);
      break;
    default:
      CV_Error(cv::Error::StsUnsupportedFormat, "CUDA::Rotate90CW: unsupported type");
  }

  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
  img = out;
}

void Rotate90CCW(cv::cuda::GpuMat& img, cv::cuda::Stream* stream) {
  if (img.empty()) return;
  cv::cuda::Stream local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;
  cv::cuda::GpuMat out;

  switch (img.type()) {
    case CV_32FC4:
      Rotate90CCW_Impl<float4>(img, out, active_stream);
      break;
    case CV_32FC3:
      Rotate90CCW_Impl<float3>(img, out, active_stream);
      break;
    case CV_32FC1:
      Rotate90CCW_Impl<float>(img, out, active_stream);
      break;
    default:
      CV_Error(cv::Error::StsUnsupportedFormat, "CUDA::Rotate90CCW: unsupported type");
  }

  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
  img = out;
}

}  // namespace CUDA
}  // namespace alcedo
