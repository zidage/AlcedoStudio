// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cstdint>
#include <stdexcept>

#include "cuda/nn/common.hpp"
#include "cuda/nn/demosaicnet_nhwc.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int   kResidualChannels = 12;
constexpr int   kCatChannels      = 6;
constexpr int   kRgbChannels      = 3;
constexpr int   kBlockSize        = 256;

__global__ void Residual1x1NhwcKernel(const float* __restrict__ input, float* __restrict__ residual,
                                      const float* __restrict__ weight,
                                      const float* __restrict__ bias, const int channels,
                                      const std::int64_t pixels) {
  constexpr int      kPixelsPerBlock = 16;
  const int          co              = static_cast<int>(threadIdx.x) % kResidualChannels;
  const int          local_pixel     = static_cast<int>(threadIdx.x) / kResidualChannels;
  const std::int64_t pixel = static_cast<std::int64_t>(blockIdx.x) * kPixelsPerBlock + local_pixel;
  if (pixel < pixels) {
    const float* in  = input + pixel * channels;
    float        acc = bias[co];
    for (int ci = 0; ci < channels; ++ci) {
      acc = fmaf(in[ci], weight[static_cast<std::int64_t>(ci) * kResidualChannels + co], acc);
    }
    residual[pixel * kResidualChannels + co] = acc;
  }
}

__global__ void UnpackCropConcatNhwcKernel(const float* __restrict__ mosaic, const int input_h,
                                           const int input_w, const float* __restrict__ residual,
                                           const int residual_h, const int    residual_w,
                                           float* __restrict__ cat, const int batch) {
  const int          up_h         = residual_h * 2;
  const int          up_w         = residual_w * 2;
  const int          crop_y       = (input_h - up_h) / 2;
  const int          crop_x       = (input_w - up_w) / 2;
  const std::int64_t pixels       = static_cast<std::int64_t>(batch) * up_h * up_w;
  const std::int64_t stride       = static_cast<std::int64_t>(blockDim.x) * gridDim.x;
  const std::int64_t mosaic_plane = static_cast<std::int64_t>(input_h) * input_w;

  for (std::int64_t pixel = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       pixel < pixels; pixel += stride) {
    std::int64_t rem = pixel;
    const int    x   = static_cast<int>(rem % up_w);
    rem /= up_w;
    const int          y            = static_cast<int>(rem % up_h);
    const int          n            = static_cast<int>(rem / up_h);

    const int          source_x     = x + crop_x;
    const int          source_y     = y + crop_y;
    const std::int64_t mosaic_pixel = static_cast<std::int64_t>(n) * kRgbChannels * mosaic_plane +
                                      static_cast<std::int64_t>(source_y) * input_w + source_x;
    const std::int64_t residual_pixel =
        ((static_cast<std::int64_t>(n) * residual_h + y / 2) * residual_w + x / 2) *
        kResidualChannels;
    float* out = cat + pixel * kCatChannels;

#pragma unroll
    for (int c = 0; c < kRgbChannels; ++c) {
      out[c]                   = mosaic[mosaic_pixel + static_cast<std::int64_t>(c) * mosaic_plane];
      const int unpack_channel = c * 4 + (y & 1) * 2 + (x & 1);
      out[kRgbChannels + c]    = residual[residual_pixel + unpack_channel];
    }
  }
}

}  // namespace

void TransformDemosaicNetResidualWeightsNhwc(const float* src_oihw, const int channels,
                                             float* dst_cio) {
  if (src_oihw == nullptr || dst_cio == nullptr || (channels != 24 && channels != 32)) {
    throw std::runtime_error("TransformDemosaicNetResidualWeightsNhwc: invalid student width");
  }
  for (int ci = 0; ci < channels; ++ci) {
    for (int co = 0; co < kResidualChannels; ++co) {
      dst_cio[static_cast<std::size_t>(ci) * kResidualChannels + co] =
          src_oihw[static_cast<std::size_t>(co) * channels + ci];
    }
  }
}

void DemosaicNetResidual1x1Nhwc(const float* input_nhwc, float* residual_nhwc,
                                const float* weight_cio, const float* bias, const int batch,
                                const int height, const int width, const int channels,
                                const cudaStream_t stream) {
  if (input_nhwc == nullptr || residual_nhwc == nullptr || weight_cio == nullptr ||
      bias == nullptr || batch < 1 || height < 1 || width < 1 ||
      (channels != 24 && channels != 32)) {
    throw std::runtime_error("DemosaicNetResidual1x1Nhwc: invalid student tensor");
  }
  const std::int64_t pixels          = static_cast<std::int64_t>(batch) * height * width;
  constexpr int      kPixelsPerBlock = 16;
  constexpr int      kThreads        = kPixelsPerBlock * kResidualChannels;
  const int          grid = static_cast<int>((pixels + kPixelsPerBlock - 1) / kPixelsPerBlock);
  Residual1x1NhwcKernel<<<grid, kThreads, 0, stream>>>(input_nhwc, residual_nhwc, weight_cio, bias,
                                                       channels, pixels);
  CheckCuda(cudaGetLastError(), "Residual1x1NhwcKernel launch");
}

void DemosaicNetUnpackCropConcatNhwc(const float* mosaic_nchw, const int input_h, const int input_w,
                                     const float* residual_nhwc, const int residual_h,
                                     const int residual_w, float* cat_nhwc, const int batch,
                                     const cudaStream_t stream) {
  const int up_h = residual_h * 2;
  const int up_w = residual_w * 2;
  if (mosaic_nchw == nullptr || residual_nhwc == nullptr || cat_nhwc == nullptr || batch < 1 ||
      residual_h < 1 || residual_w < 1 || input_h < up_h || input_w < up_w ||
      ((input_h - up_h) & 1) != 0 || ((input_w - up_w) & 1) != 0) {
    throw std::runtime_error("DemosaicNetUnpackCropConcatNhwc: invalid centered geometry");
  }
  const std::int64_t pixels = static_cast<std::int64_t>(batch) * up_h * up_w;
  const int          grid   = ChooseGridSize(pixels, kBlockSize);
  UnpackCropConcatNhwcKernel<<<grid, kBlockSize, 0, stream>>>(
      mosaic_nchw, input_h, input_w, residual_nhwc, residual_h, residual_w, cat_nhwc, batch);
  CheckCuda(cudaGetLastError(), "UnpackCropConcatNhwcKernel launch");
}

}  // namespace alcedo::cuda::nn
