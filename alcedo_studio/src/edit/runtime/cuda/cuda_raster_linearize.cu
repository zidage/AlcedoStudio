//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <stdexcept>
#include <string>

#include "edit/runtime/cuda/cuda_raster_linearize.hpp"
#include "edit/runtime/raster_linearize_math.h"

namespace alcedo {
namespace {

template <typename T>
__global__ void LinearizeRasterKernel(const T* source, std::uint32_t pixel_count,
                                      const float* params, float* linear_rgb) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) {
    return;
  }
  const T*    pixel = source + static_cast<std::size_t>(index) * 4;
  const RlRgb rgb   = RlLinearize(static_cast<float>(pixel[0]), static_cast<float>(pixel[1]),
                                  static_cast<float>(pixel[2]), params);
  float*      out   = linear_rgb + static_cast<std::size_t>(index) * 3;
  out[0]            = rgb.r;
  out[1]            = rgb.g;
  out[2]            = rgb.b;
}

}  // namespace

void LaunchCudaLinearizeRaster(const void* source, HostPixelFormat format, std::uint32_t width,
                               std::uint32_t height, const float* params, float* linear_rgb,
                               cudaStream_t stream) {
  const std::uint32_t     pixels = width * height;
  constexpr std::uint32_t kBlock = 256;
  const std::uint32_t     grid   = (pixels + kBlock - 1) / kBlock;
  switch (format) {
    case HostPixelFormat::U8Rgba:
      LinearizeRasterKernel<<<grid, kBlock, 0, stream>>>(static_cast<const std::uint8_t*>(source),
                                                         pixels, params, linear_rgb);
      break;
    case HostPixelFormat::U16Rgba:
      LinearizeRasterKernel<<<grid, kBlock, 0, stream>>>(static_cast<const std::uint16_t*>(source),
                                                         pixels, params, linear_rgb);
      break;
    case HostPixelFormat::F32Rgba:
      LinearizeRasterKernel<<<grid, kBlock, 0, stream>>>(static_cast<const float*>(source), pixels,
                                                         params, linear_rgb);
      break;
    case HostPixelFormat::U16Cfa:
      throw std::runtime_error("LaunchCudaLinearizeRaster: a CFA plane is not raster input");
  }
  if (const auto status = ::cudaGetLastError(); status != cudaSuccess) {
    throw std::runtime_error(std::string("LaunchCudaLinearizeRaster: ") +
                             ::cudaGetErrorString(status));
  }
}

}  // namespace alcedo
