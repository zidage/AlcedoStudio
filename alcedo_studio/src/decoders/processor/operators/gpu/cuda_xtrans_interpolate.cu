//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/* Taken from https://www.cybercom.net/~dcoffin/dcraw/dcraw.c */
/* Which itself attributes this algorithm to "Frank Markesteijn" */

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <stdexcept>

#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"
#include "decoders/processor/operators/gpu/cuda_xtrans_interpolate.hpp"

namespace alcedo {
namespace CUDA {
namespace {

__device__ __forceinline__ int ClampCoord(const int value, const int limit) {
  return max(0, min(value, limit - 1));
}

__device__ __forceinline__ float SafeRead(const cv::cuda::PtrStep<float> tex, const int width,
                                          const int height, const int y, const int x) {
  return tex.ptr(ClampCoord(y, height))[ClampCoord(x, width)];
}

__device__ float FindDirectionalGreen(const cv::cuda::PtrStep<float> raw, const int width,
                                      const int height, const XTransPattern6x6& pattern,
                                      const int green_radius, const int y, const int x) {
  float left      = SafeRead(raw, width, height, y, x);
  float right     = left;
  float up        = left;
  float down      = left;

  bool  has_left  = false;
  bool  has_right = false;
  bool  has_up    = false;
  bool  has_down  = false;

  for (int radius = 1; radius <= green_radius && (!has_left || !has_right); ++radius) {
    if (!has_left && RgbColorAt(pattern, y, x - radius) == 1) {
      left     = SafeRead(raw, width, height, y, x - radius);
      has_left = true;
    }
    if (!has_right && RgbColorAt(pattern, y, x + radius) == 1) {
      right     = SafeRead(raw, width, height, y, x + radius);
      has_right = true;
    }
  }

  for (int radius = 1; radius <= green_radius && (!has_up || !has_down); ++radius) {
    if (!has_up && RgbColorAt(pattern, y - radius, x) == 1) {
      up     = SafeRead(raw, width, height, y - radius, x);
      has_up = true;
    }
    if (!has_down && RgbColorAt(pattern, y + radius, x) == 1) {
      down     = SafeRead(raw, width, height, y + radius, x);
      has_down = true;
    }
  }

  if (has_left && has_right && has_up && has_down) {
    const float horizontal_grad = fabsf(left - right);
    const float vertical_grad   = fabsf(up - down);
    return horizontal_grad <= vertical_grad ? 0.5f * (left + right) : 0.5f * (up + down);
  }
  if (has_left && has_right) {
    return 0.5f * (left + right);
  }
  if (has_up && has_down) {
    return 0.5f * (up + down);
  }

  float sum   = 0.0f;
  int   count = 0;
  for (int radius = 1; radius <= green_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (RgbColorAt(pattern, y + dy, x + dx) != 1) {
          continue;
        }
        sum += SafeRead(raw, width, height, y + dy, x + dx);
        ++count;
      }
    }
    if (count > 0) {
      break;
    }
  }

  return count > 0 ? sum / static_cast<float>(count) : SafeRead(raw, width, height, y, x);
}

__device__ float EstimateMissingChannel(const cv::cuda::PtrStep<float> raw,
                                        const cv::cuda::PtrStep<float> green, const int width,
                                        const int height, const XTransPattern6x6& pattern,
                                        const int rb_radius, const int y, const int x,
                                        const int target_color, const float current_green) {
  float sum  = 0.0f;
  float wsum = 0.0f;

  for (int radius = 1; radius <= rb_radius; ++radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        if (max(abs(dx), abs(dy)) != radius) {
          continue;
        }
        if (RgbColorAt(pattern, y + dy, x + dx) != target_color) {
          continue;
        }

        const float neigh_raw   = SafeRead(raw, width, height, y + dy, x + dx);
        const float neigh_green = SafeRead(green, width, height, y + dy, x + dx);
        const float weight      = 1.0f / static_cast<float>(abs(dx) + abs(dy));
        sum += (neigh_raw - neigh_green) * weight;
        wsum += weight;
      }
    }
    if (wsum > 0.0f) {
      break;
    }
  }

  if (wsum == 0.0f) {
    return current_green;
  }

  return fmaxf(0.0f, current_green + sum / wsum);
}

__global__ void XTransGreenKernel(const cv::cuda::PtrStep<float> raw,
                                  cv::cuda::PtrStep<float> green, const int width, const int height,
                                  XTransPattern6x6 pattern, const int green_radius) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  const int   color = RgbColorAt(pattern, y, x);
  const float value = color == 1
                          ? raw.ptr(y)[x]
                          : FindDirectionalGreen(raw, width, height, pattern, green_radius, y, x);
  green.ptr(y)[x]   = value;
}

__global__ void XTransRgbKernel(const cv::cuda::PtrStep<float> raw,
                                const cv::cuda::PtrStep<float> green,
                                cv::cuda::PtrStep<float3> output, const int width, const int height,
                                XTransPattern6x6 pattern, const int rb_radius) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  const int   color       = RgbColorAt(pattern, y, x);
  const float raw_value   = raw.ptr(y)[x];
  const float green_value = green.ptr(y)[x];

  float       r           = color == 0 ? raw_value
                                       : EstimateMissingChannel(raw, green, width, height, pattern, rb_radius, y, x,
                                                                0, green_value);
  float       g           = color == 1 ? raw_value : green_value;
  float       b           = color == 2 ? raw_value
                                       : EstimateMissingChannel(raw, green, width, height, pattern, rb_radius, y, x,
                                                                2, green_value);

  output.ptr(y)[x]        = make_float3(r, g, b);
}

}  // namespace

void XTransToRGB_Ref(const cv::cuda::GpuMat& raw, cv::cuda::GpuMat& green, cv::cuda::GpuMat& output,
                     const XTransPattern6x6& pattern, int passes, cv::cuda::Stream* stream) {
  if (raw.empty()) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: input image is empty");
  }
  if (raw.type() != CV_32FC1) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: expected CV_32FC1 raw input");
  }
  if (green.type() != CV_32FC1 || green.size() != raw.size() || green.empty()) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: green must be a preallocated CV_32FC1 of raw size");
  }
  if (output.type() != CV_32FC3 || output.size() != raw.size() || output.empty()) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: output must be a preallocated CV_32FC3 of raw size");
  }

  const int width  = raw.cols;
  const int height = raw.rows;
  if (width <= 0 || height <= 0) {
    return;
  }

  cv::cuda::Stream  local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;
  cudaStream_t      cuda_stream   = cv::cuda::StreamAccessor::getStream(active_stream);

  const dim3 threads(32, 8);
  const dim3 blocks((width + threads.x - 1) / threads.x, (height + threads.y - 1) / threads.y);
  const int  green_radius = 3;
  const int  rb_radius    = std::max(passes, 1) > 1 ? 4 : 3;

  XTransGreenKernel<<<blocks, threads, 0, cuda_stream>>>(raw, green, width, height, pattern,
                                                         green_radius);
  CUDA_CHECK(cudaGetLastError());
  XTransRgbKernel<<<blocks, threads, 0, cuda_stream>>>(raw, green, output, width, height, pattern,
                                                       rb_radius);
  CUDA_CHECK(cudaGetLastError());
  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
}

void XTransToRGB_Ref(cv::cuda::GpuMat& image, const XTransPattern6x6& pattern, int passes) {
  if (image.empty()) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: input image is empty");
  }
  if (image.type() != CV_32FC1) {
    throw std::runtime_error("CUDA::XTransToRGB_Ref: expected CV_32FC1 raw input");
  }

  const int width  = image.cols;
  const int height = image.rows;
  if (width <= 0 || height <= 0) {
    return;
  }

  cv::cuda::GpuMat green(height, width, CV_32FC1);
  cv::cuda::GpuMat output(height, width, CV_32FC3);
  XTransToRGB_Ref(image, green, output, pattern, passes, nullptr);
  image = output;
}

}  // namespace CUDA
}  // namespace alcedo
