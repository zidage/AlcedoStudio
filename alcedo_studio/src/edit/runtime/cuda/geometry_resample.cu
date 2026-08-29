//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/geometry_resample_pass.hpp"

#include <stdexcept>
#include <string>

#include "cuda/cuda_check.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

__device__ auto ReadBorder(const float4* src, int width, int height, int x, int y, float4 border)
    -> float4 {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return border;
  }
  return src[y * width + x];
}

__device__ auto BilinearSample(const float4* src, int width, int height, float sx, float sy,
                               float4 border) -> float4 {
  const float px = sx - 0.5f;
  const float py = sy - 0.5f;
  const int   x0 = static_cast<int>(floorf(px));
  const int   y0 = static_cast<int>(floorf(py));
  const float fx = px - static_cast<float>(x0);
  const float fy = py - static_cast<float>(y0);
  const float4 p00 = ReadBorder(src, width, height, x0, y0, border);
  const float4 p10 = ReadBorder(src, width, height, x0 + 1, y0, border);
  const float4 p01 = ReadBorder(src, width, height, x0, y0 + 1, border);
  const float4 p11 = ReadBorder(src, width, height, x0 + 1, y0 + 1, border);
  const float  w00 = (1.0f - fx) * (1.0f - fy);
  const float  w10 = fx * (1.0f - fy);
  const float  w01 = (1.0f - fx) * fy;
  const float  w11 = fx * fy;
  float4       out;
  out.x = w00 * p00.x + w10 * p10.x + w01 * p01.x + w11 * p11.x;
  out.y = w00 * p00.y + w10 * p10.y + w01 * p01.y + w11 * p11.y;
  out.z = w00 * p00.z + w10 * p10.z + w01 * p01.z + w11 * p11.z;
  out.w = w00 * p00.w + w10 * p10.w + w01 * p01.w + w11 * p11.w;
  return out;
}

__device__ auto CubicWeight(float x) -> float {
  x = fabsf(x);
  if (x < 1.0f) {
    return ((1.5f * x - 2.5f) * x) * x + 1.0f;
  }
  if (x < 2.0f) {
    return (((-0.5f * x + 2.5f) * x) - 4.0f) * x + 2.0f;
  }
  return 0.0f;
}

__device__ auto BicubicSample(const float4* src, int width, int height, float sx, float sy,
                              float4 border) -> float4 {
  const float px = sx - 0.5f;
  const float py = sy - 0.5f;
  const int   x0 = static_cast<int>(floorf(px));
  const int   y0 = static_cast<int>(floorf(py));
  const float fx = px - static_cast<float>(x0);
  const float fy = py - static_cast<float>(y0);
  float4      acc = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  float       wsum = 0.0f;
  for (int j = -1; j <= 2; ++j) {
    const float wy = CubicWeight(static_cast<float>(j) - fy);
    for (int i = -1; i <= 2; ++i) {
      const float  wx = CubicWeight(static_cast<float>(i) - fx);
      const float  w  = wx * wy;
      const float4 p  = ReadBorder(src, width, height, x0 + i, y0 + j, border);
      acc.x += w * p.x;
      acc.y += w * p.y;
      acc.z += w * p.z;
      acc.w += w * p.w;
      wsum += w;
    }
  }
  if (wsum <= 1.0e-8f) {
    return border;
  }
  acc.x /= wsum;
  acc.y /= wsum;
  acc.z /= wsum;
  acc.w /= wsum;
  return acc;
}

__global__ void GeometryResampleKernel(const float4* src, float4* dst, int decoded_w, int decoded_h,
                                       int render_w, int render_h, float m00, float m01, float m02,
                                       float m10, float m11, float m12, float4 border,
                                       int use_bicubic) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= render_w || y >= render_h) {
    return;
  }
  const float cx = static_cast<float>(x) + 0.5f;
  const float cy = static_cast<float>(y) + 0.5f;
  const float sx = m00 * cx + m01 * cy + m02;
  const float sy = m10 * cx + m11 * cy + m12;
  const float4 pixel = use_bicubic ? BicubicSample(src, decoded_w, decoded_h, sx, sy, border)
                                   : BilinearSample(src, decoded_w, decoded_h, sx, sy, border);
  dst[y * render_w + x] = pixel;
}

}  // namespace

void GeometryResamplePass::Encode(const ResolvedRenderGeometry& geometry,
                                  const CudaBackend::Texture2D& src, CudaBackend::Texture2D& dst,
                                  CudaCommandContext& command_context) {
  if (src.Format() != TextureFormat::Rgba32f || dst.Format() != TextureFormat::Rgba32f) {
    throw std::runtime_error("GeometryResamplePass::Encode: textures must be RGBA32F");
  }
  if (src.Width() != geometry.decoded_extent.width ||
      src.Height() != geometry.decoded_extent.height) {
    throw std::runtime_error(
        "GeometryResamplePass::Encode: src size " + std::to_string(src.Width()) + "x" +
        std::to_string(src.Height()) + " must match decoded_extent " +
        std::to_string(geometry.decoded_extent.width) + "x" +
        std::to_string(geometry.decoded_extent.height));
  }
  if (dst.Width() != geometry.render_extent.width ||
      dst.Height() != geometry.render_extent.height) {
    throw std::runtime_error(
        "GeometryResamplePass::Encode: dst size " + std::to_string(dst.Width()) + "x" +
        std::to_string(dst.Height()) + " must match render_extent " +
        std::to_string(geometry.render_extent.width) + "x" +
        std::to_string(geometry.render_extent.height));
  }
  if (src.DevicePointer() == nullptr || dst.DevicePointer() == nullptr) {
    throw std::runtime_error("GeometryResamplePass::Encode: empty texture");
  }

  const auto& gpu = geometry.gpu_data;
  const dim3  block(16, 16);
  const dim3  grid((gpu.render_width + block.x - 1) / block.x,
                   (gpu.render_height + block.y - 1) / block.y);
  if (grid.x == 0 || grid.y == 0) {
    throw std::runtime_error("GeometryResamplePass::Encode: invalid launch grid");
  }

  const float4 border =
      make_float4(gpu.border_rgba[0], gpu.border_rgba[1], gpu.border_rgba[2], gpu.border_rgba[3]);
  const int use_bicubic = geometry.filter == TextureFilter::Bicubic ? 1 : 0;

  GeometryResampleKernel<<<grid, block, 0, command_context.Stream()>>>(
      static_cast<const float4*>(src.DevicePointer()), static_cast<float4*>(dst.DevicePointer()),
      static_cast<int>(gpu.decoded_width), static_cast<int>(gpu.decoded_height),
      static_cast<int>(gpu.render_width), static_cast<int>(gpu.render_height), gpu.render_to_decoded[0],
      gpu.render_to_decoded[1], gpu.render_to_decoded[2], gpu.render_to_decoded[3],
      gpu.render_to_decoded[4], gpu.render_to_decoded[5], border, use_bicubic);
  cuda::CheckCuda(::cudaGetLastError(), "GeometryResamplePass::Encode launch");
  ++launch_count_;
}

}  // namespace alcedo
