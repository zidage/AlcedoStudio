//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <stdexcept>
#include <type_traits>

#include "decoders/processor/operators/gpu/cuda_dng_warp.hpp"
#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"

namespace alcedo {
namespace CUDA {
namespace {

struct WarpRectilinearParams {
  int   coefficient_set_count  = 0;
  float coefficient_sets[3][6] = {};
  float center_x               = 0.5f;
  float center_y               = 0.5f;
};

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

__device__ auto ReadWithBorder(const cv::cuda::PtrStepSz<float3>& src, const int x, const int y)
    -> float3 {
  if (x < 0 || y < 0 || x >= src.cols || y >= src.rows) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }
  return src(y, x);
}

__device__ auto ReadWithBorder(const cv::cuda::PtrStepSz<float4>& src, const int x, const int y)
    -> float4 {
  if (x < 0 || y < 0 || x >= src.cols || y >= src.rows) {
    return make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return src(y, x);
}

template <typename PixelT>
__device__ auto BilinearSample(const cv::cuda::PtrStepSz<PixelT>& src, const float sx,
                               const float sy) -> PixelT {
  const int    x0  = static_cast<int>(floorf(sx));
  const int    y0  = static_cast<int>(floorf(sy));
  const int    x1  = x0 + 1;
  const int    y1  = y0 + 1;
  const float  fx  = sx - static_cast<float>(x0);
  const float  fy  = sy - static_cast<float>(y0);
  const float  w00 = (1.0f - fx) * (1.0f - fy);
  const float  w10 = fx * (1.0f - fy);
  const float  w01 = (1.0f - fx) * fy;
  const float  w11 = fx * fy;

  const PixelT p00 = ReadWithBorder(src, x0, y0);
  const PixelT p10 = ReadWithBorder(src, x1, y0);
  const PixelT p01 = ReadWithBorder(src, x0, y1);
  const PixelT p11 = ReadWithBorder(src, x1, y1);

  PixelT       out{};
  if constexpr (std::is_same_v<PixelT, float3>) {
    out.x = p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11;
    out.y = p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11;
    out.z = p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11;
  } else {
    out.x = p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11;
    out.y = p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11;
    out.z = p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11;
    out.w = p00.w * w00 + p10.w * w10 + p01.w * w01 + p11.w * w11;
  }
  return out;
}

__device__ auto WarpSourceCoord(const int x, const int y, const int plane, const int width,
                                const int height, const WarpRectilinearParams& p) -> float2 {
  const float x0 = 0.0f;
  const float y0 = 0.0f;
  const float x1 = static_cast<float>(max(width - 1, 0));
  const float y1 = static_cast<float>(max(height - 1, 0));
  const float cx = x0 + p.center_x * (x1 - x0);
  const float cy = y0 + p.center_y * (y1 - y0);
  const float mx = fmaxf(fabsf(x0 - cx), fabsf(x1 - cx));
  const float my = fmaxf(fabsf(y0 - cy), fabsf(y1 - cy));
  const float m  = sqrtf(mx * mx + my * my);
  if (m <= 1e-8f) {
    return make_float2(static_cast<float>(x), static_cast<float>(y));
  }

  const int    set_index = (p.coefficient_set_count <= 1) ? 0 : min(max(plane, 0), 2);
  const float* coeffs    = p.coefficient_sets[set_index];
  const float  dx        = (static_cast<float>(x) - cx) / m;
  const float  dy        = (static_cast<float>(y) - cy) / m;
  const float  r2        = dx * dx + dy * dy;
  const float  f   = coeffs[0] + coeffs[1] * r2 + coeffs[2] * r2 * r2 + coeffs[3] * r2 * r2 * r2;
  const float  dxr = f * dx;
  const float  dyr = f * dy;
  const float  dxt = coeffs[4] * (2.0f * dx * dy) + coeffs[5] * (r2 + 2.0f * dx * dx);
  const float  dyt = coeffs[5] * (2.0f * dx * dy) + coeffs[4] * (r2 + 2.0f * dy * dy);
  return make_float2(cx + m * (dxr + dxt), cy + m * (dyr + dyt));
}

__device__ auto BilinearSampleChannel(const cv::cuda::PtrStepSz<float3>& src, const float sx,
                                      const float sy, const int channel) -> float {
  const float3 pixel = BilinearSample(src, sx, sy);
  if (channel == 0) return pixel.x;
  if (channel == 1) return pixel.y;
  return pixel.z;
}

__device__ auto BilinearSampleChannel(const cv::cuda::PtrStepSz<float4>& src, const float sx,
                                      const float sy, const int channel) -> float {
  const float4 pixel = BilinearSample(src, sx, sy);
  if (channel == 0) return pixel.x;
  if (channel == 1) return pixel.y;
  if (channel == 2) return pixel.z;
  return pixel.w;
}

template <typename PixelT>
__global__ void WarpRectilinearKernel(const cv::cuda::PtrStepSz<PixelT> src,
                                      cv::cuda::PtrStepSz<PixelT>       dst,
                                      const WarpRectilinearParams       p) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  if constexpr (std::is_same_v<PixelT, float3>) {
    const float2 red   = WarpSourceCoord(x, y, 0, dst.cols, dst.rows, p);
    const float2 green = WarpSourceCoord(x, y, 1, dst.cols, dst.rows, p);
    const float2 blue  = WarpSourceCoord(x, y, 2, dst.cols, dst.rows, p);
    dst(y, x)          = make_float3(BilinearSampleChannel(src, red.x, red.y, 0),
                                     BilinearSampleChannel(src, green.x, green.y, 1),
                                     BilinearSampleChannel(src, blue.x, blue.y, 2));
  } else {
    const float2 red   = WarpSourceCoord(x, y, 0, dst.cols, dst.rows, p);
    const float2 green = WarpSourceCoord(x, y, 1, dst.cols, dst.rows, p);
    const float2 blue  = WarpSourceCoord(x, y, 2, dst.cols, dst.rows, p);
    dst(y, x)          = make_float4(BilinearSampleChannel(src, red.x, red.y, 0),
                                     BilinearSampleChannel(src, green.x, green.y, 1),
                                     BilinearSampleChannel(src, blue.x, blue.y, 2),
                                     BilinearSampleChannel(src, green.x, green.y, 3));
  }
}

auto BuildParams(const dng::WarpRectilinear& warp) -> WarpRectilinearParams {
  WarpRectilinearParams params{};
  params.coefficient_set_count = static_cast<int>(warp.coefficient_set_count);
  params.center_x              = static_cast<float>(warp.center_x);
  params.center_y              = static_cast<float>(warp.center_y);
  for (size_t set = 0; set < warp.coefficient_sets.size(); ++set) {
    for (size_t term = 0; term < warp.coefficient_sets[set].size(); ++term) {
      params.coefficient_sets[set][term] = static_cast<float>(warp.coefficient_sets[set][term]);
    }
  }
  return params;
}

template <typename PixelT>
void LaunchWarp(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                const dng::WarpRectilinear& warp, cudaStream_t stream) {
  const dim3 block(32, 8);
  const dim3 grid((dst.cols + block.x - 1) / block.x, (dst.rows + block.y - 1) / block.y);
  WarpRectilinearKernel<PixelT><<<grid, block, 0, stream>>>(src, dst, BuildParams(warp));
  CUDA_CHECK(cudaGetLastError());
  MaybeSync(stream);
}

}  // namespace

void WarpDngRectilinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                        const dng::WarpRectilinear& warp, cv::cuda::Stream* stream) {
  if (src.empty() || dst.empty()) {
    throw std::runtime_error("CUDA::WarpDngRectilinear requires non-empty images");
  }
  if (src.size() != dst.size() || src.type() != dst.type()) {
    throw std::runtime_error("CUDA::WarpDngRectilinear requires matching source and destination");
  }
  const cudaStream_t cuda_stream = GetCudaStream(stream);
  if (src.type() == CV_32FC3) {
    LaunchWarp<float3>(src, dst, warp, cuda_stream);
    return;
  }
  if (src.type() == CV_32FC4) {
    LaunchWarp<float4>(src, dst, warp, cuda_stream);
    return;
  }
  throw std::runtime_error("CUDA::WarpDngRectilinear expects CV_32FC3/CV_32FC4 input");
}

void ApplyDngWarpRectilinear(cv::cuda::GpuMat& img, const dng::WarpRectilinear& warp,
                             cv::cuda::Stream* stream) {
  if (img.empty()) {
    return;
  }
  const cudaStream_t cuda_stream = GetCudaStream(stream);
  cv::cuda::GpuMat   out(img.rows, img.cols, img.type());
  if (img.type() == CV_32FC3) {
    LaunchWarp<float3>(img, out, warp, cuda_stream);
    img = std::move(out);
    return;
  }
  if (img.type() == CV_32FC4) {
    LaunchWarp<float4>(img, out, warp, cuda_stream);
    img = std::move(out);
    return;
  }
  throw std::runtime_error("CUDA::ApplyDngWarpRectilinear expects CV_32FC3/CV_32FC4 input");
}

}  // namespace CUDA
}  // namespace alcedo
