//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <cuda_runtime_api.h>

#include <cmath>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/operators/geometry/cuda_geometry_ops.hpp"

namespace alcedo {
namespace CUDA {
namespace {

constexpr float kEps = 1e-8f;

// Geometry kernels must not abort the process (the shared CUDA_CHECK helper
// calls exit). Surface launch/runtime failures as exceptions so the editor
// render path can fail a single job and recover.
#define ALCEDO_CUDA_GEOMETRY_CHECK(call)                                                          \
  do {                                                                                            \
    const cudaError_t alcedo_cuda_err = (call);                                                   \
    if (alcedo_cuda_err != cudaSuccess) {                                                         \
      throw std::runtime_error(std::string("CUDA geometry error: ") +                             \
                               cudaGetErrorString(alcedo_cuda_err) + " at " + __FILE__ + ":" +  \
                               std::to_string(__LINE__));                                         \
    }                                                                                             \
  } while (0)

void ClearStickyCudaError() {
  // A prior async failure can stick as "invalid argument" on the next kernel
  // launch check. Clear it so geometry operators report their own failures.
  (void)cudaGetLastError();
}

void ValidateResizeLaunch(const cv::cuda::GpuMat& src, cv::Size dst_size, const char* api) {
  if (src.empty() || src.data == nullptr || src.cols <= 0 || src.rows <= 0) {
    throw std::runtime_error(std::string(api) + ": source GpuMat is empty");
  }
  if (dst_size.width <= 0 || dst_size.height <= 0) {
    throw std::runtime_error(std::string(api) + ": destination size must be positive");
  }
  if (src.step < static_cast<size_t>(src.cols) * src.elemSize()) {
    throw std::runtime_error(std::string(api) + ": source step is smaller than row byte width");
  }
}

template <typename T>
struct PixelOps;

template <>
struct PixelOps<float> {
  using Pixel = float;
  using Acc   = float;

  __device__ static auto Zero() -> Acc { return 0.0f; }
  __device__ static auto AddMul(Acc acc, Pixel value, float w) -> Acc {
    return fmaf(value, w, acc);
  }
  __device__ static auto Div(Acc acc, float denom) -> Pixel { return acc / denom; }
};

template <>
struct PixelOps<float3> {
  using Pixel = float3;
  using Acc   = float3;

  __device__ static auto Zero() -> Acc { return make_float3(0.0f, 0.0f, 0.0f); }

  __device__ static auto AddMul(Acc acc, Pixel value, float w) -> Acc {
    acc.x += value.x * w;
    acc.y += value.y * w;
    acc.z += value.z * w;
    return acc;
  }

  __device__ static auto Div(Acc acc, float denom) -> Pixel {
    return make_float3(acc.x / denom, acc.y / denom, acc.z / denom);
  }
};

template <>
struct PixelOps<float4> {
  using Pixel = float4;
  using Acc   = float4;

  __device__ static auto Zero() -> Acc { return make_float4(0.0f, 0.0f, 0.0f, 0.0f); }

  __device__ static auto AddMul(Acc acc, Pixel value, float w) -> Acc {
    acc.x += value.x * w;
    acc.y += value.y * w;
    acc.z += value.z * w;
    acc.w += value.w * w;
    return acc;
  }

  __device__ static auto Div(Acc acc, float denom) -> Pixel {
    return make_float4(acc.x / denom, acc.y / denom, acc.z / denom, acc.w / denom);
  }
};

template <typename PixelT>
__device__ auto ReadOrBorder(const cv::cuda::PtrStepSz<PixelT>& src, int x, int y, PixelT border)
    -> PixelT {
  if (x < 0 || y < 0 || x >= src.cols || y >= src.rows) {
    return border;
  }
  return src(y, x);
}

template <typename PixelT>
__device__ auto BilinearSample(const cv::cuda::PtrStepSz<PixelT>& src, float sx, float sy,
                               PixelT border) -> PixelT {
  using Ops        = PixelOps<PixelT>;
  using Acc        = typename Ops::Acc;

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

  const PixelT p00 = ReadOrBorder(src, x0, y0, border);
  const PixelT p10 = ReadOrBorder(src, x1, y0, border);
  const PixelT p01 = ReadOrBorder(src, x0, y1, border);
  const PixelT p11 = ReadOrBorder(src, x1, y1, border);

  Acc          acc = Ops::Zero();
  acc              = Ops::AddMul(acc, p00, w00);
  acc              = Ops::AddMul(acc, p10, w10);
  acc              = Ops::AddMul(acc, p01, w01);
  acc              = Ops::AddMul(acc, p11, w11);
  return Ops::Div(acc, 1.0f);
}

template <typename PixelT>
__global__ void Downsample2xBoxKernel(const cv::cuda::PtrStepSz<PixelT> src,
                                      cv::cuda::PtrStepSz<PixelT>       dst) {
  using Ops   = PixelOps<PixelT>;
  using Acc   = typename Ops::Acc;

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  const int sx0   = x * 2;
  const int sy0   = y * 2;

  Acc       acc   = Ops::Zero();
  float     total = 0.f;

  for (int ky = 0; ky < 2; ++ky) {
    const int sy = sy0 + ky;
    if (sy >= src.rows) {
      continue;
    }

    for (int kx = 0; kx < 2; ++kx) {
      const int sx = sx0 + kx;
      if (sx >= src.cols) {
        continue;
      }

      acc = Ops::AddMul(acc, src(sy, sx), 1.f);
      total += 1.0f;
    }
  }

  if (total <= kEps) {
    return;
  }

  dst(y, x) = Ops::Div(acc, total);
}

template <typename PixelT>
__global__ void ResizeAreaKernel(const cv::cuda::PtrStepSz<PixelT> src,
                                 cv::cuda::PtrStepSz<PixelT> dst, float scale_x, float scale_y) {
  using Ops   = PixelOps<PixelT>;
  using Acc   = typename Ops::Acc;

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  const float sx0   = static_cast<float>(x) * scale_x;
  const float sx1   = static_cast<float>(x + 1) * scale_x;
  const float sy0   = static_cast<float>(y) * scale_y;
  const float sy1   = static_cast<float>(y + 1) * scale_y;

  const int   ix0   = max(0, static_cast<int>(floorf(sx0)));
  const int   ix1   = min(src.cols, static_cast<int>(ceilf(sx1)));
  const int   iy0   = max(0, static_cast<int>(floorf(sy0)));
  const int   iy1   = min(src.rows, static_cast<int>(ceilf(sy1)));

  Acc         acc   = Ops::Zero();
  float       total = 0.0f;

  for (int yy = iy0; yy < iy1; ++yy) {
    const float yy0 = fmaxf(sy0, static_cast<float>(yy));
    const float yy1 = fminf(sy1, static_cast<float>(yy + 1));
    const float wy  = fmaxf(0.0f, yy1 - yy0);
    if (wy <= 0.0f) {
      continue;
    }

    for (int xx = ix0; xx < ix1; ++xx) {
      const float xx0 = fmaxf(sx0, static_cast<float>(xx));
      const float xx1 = fminf(sx1, static_cast<float>(xx + 1));
      const float wx  = fmaxf(0.0f, xx1 - xx0);
      const float w   = wx * wy;
      if (w <= 0.0f) {
        continue;
      }

      acc = Ops::AddMul(acc, src(yy, xx), w);
      total += w;
    }
  }

  if (total <= kEps) {
    const int sx = min(max(static_cast<int>(sx0), 0), src.cols - 1);
    const int sy = min(max(static_cast<int>(sy0), 0), src.rows - 1);
    dst(y, x)    = src(sy, sx);
    return;
  }

  dst(y, x) = Ops::Div(acc, total);
}

template <typename PixelT>
void Downsample2xBoxTyped(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst) {
  ValidateResizeLaunch(src, cv::Size((src.cols + 1) / 2, (src.rows + 1) / 2),
                       "CUDA::Downsample2xBox");
  const int dst_w = (src.cols + 1) / 2;
  const int dst_h = (src.rows + 1) / 2;

  dst.create(dst_h, dst_w, src.type());
  if (dst.empty()) {
    return;
  }

  const dim3 block(16, 16);
  const dim3 grid((dst.cols + block.x - 1) / block.x, (dst.rows + block.y - 1) / block.y);

  ClearStickyCudaError();
  Downsample2xBoxKernel<PixelT><<<grid, block>>>(src, dst);
  ALCEDO_CUDA_GEOMETRY_CHECK(cudaGetLastError());
}

template <typename PixelT>
__global__ void ResizeLinearKernel(const cv::cuda::PtrStepSz<PixelT> src,
                                   cv::cuda::PtrStepSz<PixelT> dst, float scale_x, float scale_y) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  const float sx = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
  const float sy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
  dst(y, x)      = BilinearSample(src, sx, sy, PixelT{});
}

template <typename PixelT>
void ResizeLinearTyped(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size) {
  ValidateResizeLaunch(src, dst_size, "CUDA::ResizeLinear");
  dst.create(dst_size.height, dst_size.width, src.type());
  if (dst.empty() || dst.data == nullptr) {
    throw std::runtime_error("CUDA::ResizeLinear: failed to allocate destination GpuMat");
  }

  const float scale_x = static_cast<float>(src.cols) / static_cast<float>(dst_size.width);
  const float scale_y = static_cast<float>(src.rows) / static_cast<float>(dst_size.height);

  const dim3 block(16, 16);
  const dim3 grid((dst_size.width + block.x - 1) / block.x,
                  (dst_size.height + block.y - 1) / block.y);
  if (grid.x == 0 || grid.y == 0) {
    throw std::runtime_error("CUDA::ResizeLinear: invalid launch grid");
  }

  ClearStickyCudaError();
  ResizeLinearKernel<PixelT><<<grid, block>>>(src, dst, scale_x, scale_y);
  ALCEDO_CUDA_GEOMETRY_CHECK(cudaGetLastError());
}

inline auto ShouldContinueDownsample2x(const cv::Size& current, const cv::Size& target) -> bool {
  return (current.width / 2 >= target.width) && (current.height / 2 >= target.height);
}

template <typename PixelT>
void ResizeProgressiveDownsampleTyped(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                                      cv::Size dst_size) {
  if (dst_size.width <= 0 || dst_size.height <= 0) {
    throw std::runtime_error("CUDA::ResizeProgressiveDownsample: destination size must be positive");
  }

  if (src.cols <= dst_size.width || src.rows <= dst_size.height) {
    ResizeLinearTyped<PixelT>(src, dst, dst_size);
    return;
  }

  cv::cuda::GpuMat  level0 = src;
  cv::cuda::GpuMat  level1;
  cv::cuda::GpuMat  level2;

  cv::cuda::GpuMat* current = &level0;
  cv::cuda::GpuMat* next    = &level1;
  cv::cuda::GpuMat* spare   = &level2;

  while (ShouldContinueDownsample2x(current->size(), dst_size)) {
    Downsample2xBoxTyped<PixelT>(*current, *next);
    std::swap(current, next);
    std::swap(next, spare);
  }

  if (current->cols == dst_size.width && current->rows == dst_size.height) {
    current->copyTo(dst);
    return;
  }

  ResizeLinearTyped<PixelT>(*current, dst, dst_size);
}

template <typename PixelT>
__global__ void WarpAffineLinearKernel(const cv::cuda::PtrStepSz<PixelT> src,
                                       cv::cuda::PtrStepSz<PixelT> dst, float m00, float m01,
                                       float m02, float m10, float m11, float m12,
                                       PixelT border_value) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dst.cols || y >= dst.rows) {
    return;
  }

  const float sx = m00 * static_cast<float>(x) + m01 * static_cast<float>(y) + m02;
  const float sy = m10 * static_cast<float>(x) + m11 * static_cast<float>(y) + m12;
  dst(y, x)      = BilinearSample(src, sx, sy, border_value);
}

template <typename PixelT>
auto MakeBorderValue(const cv::Scalar& scalar) -> PixelT;

template <>
auto MakeBorderValue<float>(const cv::Scalar& scalar) -> float {
  return static_cast<float>(scalar[0]);
}

template <>
auto MakeBorderValue<float3>(const cv::Scalar& scalar) -> float3 {
  return make_float3(static_cast<float>(scalar[0]), static_cast<float>(scalar[1]),
                     static_cast<float>(scalar[2]));
}

template <>
auto MakeBorderValue<float4>(const cv::Scalar& scalar) -> float4 {
  return make_float4(static_cast<float>(scalar[0]), static_cast<float>(scalar[1]),
                     static_cast<float>(scalar[2]), static_cast<float>(scalar[3]));
}

template <typename PixelT>
void ResizeAreaApproxTyped(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size) {
  ValidateResizeLaunch(src, dst_size, "CUDA::ResizeAreaApprox");
  dst.create(dst_size.height, dst_size.width, src.type());
  if (dst.empty() || dst.data == nullptr) {
    throw std::runtime_error("CUDA::ResizeAreaApprox: failed to allocate destination GpuMat");
  }

  const float scale_x = static_cast<float>(src.cols) / static_cast<float>(dst_size.width);
  const float scale_y = static_cast<float>(src.rows) / static_cast<float>(dst_size.height);

  const dim3 block(16, 16);
  const dim3 grid((dst_size.width + block.x - 1) / block.x,
                  (dst_size.height + block.y - 1) / block.y);
  if (grid.x == 0 || grid.y == 0) {
    throw std::runtime_error("CUDA::ResizeAreaApprox: invalid launch grid");
  }

  ClearStickyCudaError();
  if (scale_x <= 1.0f || scale_y <= 1.0f) {
    ResizeLinearKernel<PixelT><<<grid, block>>>(src, dst, scale_x, scale_y);
    ALCEDO_CUDA_GEOMETRY_CHECK(cudaGetLastError());
    return;
  }

  ResizeAreaKernel<PixelT><<<grid, block>>>(src, dst, scale_x, scale_y);
  ALCEDO_CUDA_GEOMETRY_CHECK(cudaGetLastError());
}

template <typename PixelT>
void WarpAffineLinearTyped(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst,
                           const cv::Mat& matrix, cv::Size out_size,
                           const cv::Scalar& border_value) {
  dst.create(out_size.height, out_size.width, src.type());
  if (out_size.width <= 0 || out_size.height <= 0) {
    return;
  }

  cv::Mat matrix_32f;
  if (matrix.type() == CV_64F) {
    matrix.convertTo(matrix_32f, CV_32F);
  } else if (matrix.type() == CV_32F) {
    matrix_32f = matrix;
  } else {
    throw std::runtime_error("CUDA::WarpAffineLinear: matrix type must be CV_32F or CV_64F");
  }
  if (matrix_32f.rows != 2 || matrix_32f.cols != 3) {
    throw std::runtime_error("CUDA::WarpAffineLinear: matrix must be 2x3");
  }

  const float m00 = matrix_32f.at<float>(0, 0);
  const float m01 = matrix_32f.at<float>(0, 1);
  const float m02 = matrix_32f.at<float>(0, 2);
  const float m10 = matrix_32f.at<float>(1, 0);
  const float m11 = matrix_32f.at<float>(1, 1);
  const float m12 = matrix_32f.at<float>(1, 2);

  const dim3  block(16, 16);
  const dim3  grid((out_size.width + block.x - 1) / block.x,
                   (out_size.height + block.y - 1) / block.y);

  ClearStickyCudaError();
  WarpAffineLinearKernel<PixelT><<<grid, block>>>(src, dst, m00, m01, m02, m10, m11, m12,
                                                  MakeBorderValue<PixelT>(border_value));
  ALCEDO_CUDA_GEOMETRY_CHECK(cudaGetLastError());
}

}  // namespace

void ResizeLinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size) {
  if (src.empty()) {
    dst.release();
    return;
  }
  if (dst_size.width <= 0 || dst_size.height <= 0) {
    throw std::runtime_error("CUDA::ResizeLinear: destination size must be positive");
  }

  switch (src.type()) {
    case CV_32FC1:
      ResizeLinearTyped<float>(src, dst, dst_size);
      return;
    case CV_32FC3:
      ResizeLinearTyped<float3>(src, dst, dst_size);
      return;
    case CV_32FC4:
      ResizeLinearTyped<float4>(src, dst, dst_size);
      return;
    default:
      throw std::runtime_error("CUDA::ResizeLinear: unsupported image type");
  }
}

void ResizeAreaApprox(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, cv::Size dst_size) {
  if (src.empty()) {
    dst.release();
    return;
  }
  if (dst_size.width <= 0 || dst_size.height <= 0) {
    throw std::runtime_error("CUDA::ResizeAreaApprox: destination size must be positive");
  }

  switch (src.type()) {
    case CV_32FC1:
      ResizeAreaApproxTyped<float>(src, dst, dst_size);
      return;
    case CV_32FC3:
      ResizeAreaApproxTyped<float3>(src, dst, dst_size);
      return;
    case CV_32FC4:
      ResizeAreaApproxTyped<float4>(src, dst, dst_size);
      return;
    default:
      throw std::runtime_error("CUDA::ResizeAreaApprox: unsupported image type");
  }
}

void WarpAffineLinear(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, const cv::Mat& matrix,
                      cv::Size out_size, const cv::Scalar& border_value) {
  if (src.empty()) {
    dst.release();
    return;
  }
  if (out_size.width <= 0 || out_size.height <= 0) {
    throw std::runtime_error("CUDA::WarpAffineLinear: output size must be positive");
  }

  switch (src.type()) {
    case CV_32FC1:
      WarpAffineLinearTyped<float>(src, dst, matrix, out_size, border_value);
      return;
    case CV_32FC3:
      WarpAffineLinearTyped<float3>(src, dst, matrix, out_size, border_value);
      return;
    case CV_32FC4:
      WarpAffineLinearTyped<float4>(src, dst, matrix, out_size, border_value);
      return;
    default:
      throw std::runtime_error("CUDA::WarpAffineLinear: unsupported image type");
  }
}

void Downsample2xBox(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst) {
  if (src.empty()) {
    dst.release();
    return;
  }

  switch (src.type()) {
    case CV_32FC1:
      Downsample2xBoxTyped<float>(src, dst);
      return;
    case CV_32FC3:
      Downsample2xBoxTyped<float3>(src, dst);
      return;
    case CV_32FC4:
      Downsample2xBoxTyped<float4>(src, dst);
      return;
    default:
      throw std::runtime_error("CUDA::Downsample2xBox: unsupported image type");
  }
}

}  // namespace CUDA
}  // namespace alcedo
