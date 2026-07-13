//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/fused_post_output.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kPostCin   = 6;
constexpr int kRgbCout   = 3;
constexpr int kOhTile    = 8;
constexpr int kOwTile    = 16;
constexpr float kGammaDecode = 2.2F;

__device__ __forceinline__ auto PowSignedDevice(const float x, const float g) -> float {
  if (x == 0.0F) {
    return 0.0F;
  }
  return copysignf(powf(fabsf(x), g), x);
}

// kPostCout holds every post channel in registers (24 Bayer / 32 X-Trans).
// Cin is fixed at 6 and loaded as one strip. Grid covers the *export* spatial
// size; crop_y/crop_x map export pixels onto the natural valid grid.
template <int kPostCout>
__global__ void FusedPostOutputKernel(const float* __restrict__ cat,
                                      const float* __restrict__ post_w,
                                      const float* __restrict__ post_b,
                                      const float* __restrict__ out_w_cio,
                                      const float* __restrict__ out_b, float* __restrict__ rgb,
                                      int N, int H, int W, int out_h, int out_w, int crop_y,
                                      int crop_x, int write_hwc, std::size_t hwc_step_floats,
                                      int apply_gamma) {
  constexpr int kInH     = kOhTile + 2;
  constexpr int kInW     = kOwTile + 2;
  constexpr int kInArea  = kInH * kInW;
  constexpr int kWElems  = kPostCout * kPostCin * 9;
  constexpr int kThreads = kOhTile * kOwTile;

  const int ox0 = static_cast<int>(blockIdx.x) * kOwTile;
  const int oy0 = static_cast<int>(blockIdx.y) * kOhTile;
  const int n   = static_cast<int>(blockIdx.z);

  const int tx  = static_cast<int>(threadIdx.x);
  const int ty  = static_cast<int>(threadIdx.y);
  const int tid = ty * kOwTile + tx;

  extern __shared__ float smem[];
  float* in_s = smem;                       // kPostCin * kInArea
  float* w_s  = smem + kPostCin * kInArea;  // kWElems

  float acc[kPostCout];
#pragma unroll
  for (int i = 0; i < kPostCout; ++i) {
    acc[i] = 0.0F;
  }

  const int  ox          = ox0 + tx;
  const int  oy          = oy0 + ty;
  const bool pixel_valid = (n < N) && (oy < out_h) && (ox < out_w);

  const std::int64_t in_stride_n = static_cast<std::int64_t>(kPostCin) * H * W;
  const std::int64_t in_stride_c = static_cast<std::int64_t>(H) * W;
  const std::int64_t w_stride_co = static_cast<std::int64_t>(kPostCin) * 9;

  const float* in_n = cat + static_cast<std::int64_t>(n) * in_stride_n;

  // Apron origin in cat: export tile origin mapped into natural post space.
  const int cat_y0 = oy0 + crop_y;
  const int cat_x0 = ox0 + crop_x;

  // ---- Cooperative load: full Cin=6 input apron ----
  for (int load = tid; load < kPostCin * kInArea; load += kThreads) {
    const int lci = load / kInArea;
    const int rem = load - lci * kInArea;
    const int lh  = rem / kInW;
    const int lw  = rem - lh * kInW;

    const int gh = cat_y0 + lh;
    const int gw = cat_x0 + lw;

    float v = 0.0F;
    if (lci < kPostCin && gh >= 0 && gh < H && gw >= 0 && gw < W) {
      v = in_n[static_cast<std::int64_t>(lci) * in_stride_c +
               static_cast<std::int64_t>(gh) * W + gw];
    }
    in_s[load] = v;
  }

  // ---- Cooperative load: full post weights [Cout,6,9] ----
  for (int load = tid; load < kWElems; load += kThreads) {
    const int k   = load % 9;
    const int tmp = load / 9;
    const int lci = tmp % kPostCin;
    const int lco = tmp / kPostCin;

    float v = 0.0F;
    if (lco < kPostCout && lci < kPostCin) {
      v = post_w[static_cast<std::int64_t>(lco) * w_stride_co +
                 static_cast<std::int64_t>(lci) * 9 + k];
    }
    w_s[load] = v;
  }
  __syncthreads();

  if (pixel_valid) {
#pragma unroll
    for (int lci = 0; lci < kPostCin; ++lci) {
      const float* base = in_s + lci * kInArea + ty * kInW + tx;
      const float  i00  = base[0];
      const float  i01  = base[1];
      const float  i02  = base[2];
      const float  i10  = base[kInW];
      const float  i11  = base[kInW + 1];
      const float  i12  = base[kInW + 2];
      const float  i20  = base[2 * kInW];
      const float  i21  = base[2 * kInW + 1];
      const float  i22  = base[2 * kInW + 2];

#pragma unroll
      for (int lco = 0; lco < kPostCout; ++lco) {
        const float* w = w_s + (lco * kPostCin + lci) * 9;
        float        a = acc[lco];
        a              = fmaf(i00, w[0], a);
        a              = fmaf(i01, w[1], a);
        a              = fmaf(i02, w[2], a);
        a              = fmaf(i10, w[3], a);
        a              = fmaf(i11, w[4], a);
        a              = fmaf(i12, w[5], a);
        a              = fmaf(i20, w[6], a);
        a              = fmaf(i21, w[7], a);
        a              = fmaf(i22, w[8], a);
        acc[lco]       = a;
      }
    }

    // Post bias + ReLU (kept in registers — never written to global).
#pragma unroll
    for (int lco = 0; lco < kPostCout; ++lco) {
      float v = acc[lco] + post_b[lco];
      acc[lco] = fmaxf(v, 0.0F);
    }

    // Output 1×1: RGB = post @ W_cio + bias.
    float rgb_v[kRgbCout];
#pragma unroll
    for (int c = 0; c < kRgbCout; ++c) {
      rgb_v[c] = out_b[c];
    }
#pragma unroll
    for (int lco = 0; lco < kPostCout; ++lco) {
      const float* w3 = out_w_cio + static_cast<std::int64_t>(lco) * kRgbCout;
      const float  p  = acc[lco];
#pragma unroll
      for (int c = 0; c < kRgbCout; ++c) {
        rgb_v[c] = fmaf(p, w3[c], rgb_v[c]);
      }
    }

    if (apply_gamma) {
#pragma unroll
      for (int c = 0; c < kRgbCout; ++c) {
        rgb_v[c] = PowSignedDevice(rgb_v[c], kGammaDecode);
      }
    }

    if (write_hwc != 0) {
      // N=1 product tile: pitched HWC float3.
      float* row = rgb + static_cast<std::int64_t>(oy) * hwc_step_floats +
                   static_cast<std::int64_t>(ox) * kRgbCout;
      row[0] = rgb_v[0];
      row[1] = rgb_v[1];
      row[2] = rgb_v[2];
    } else {
      const std::int64_t plane = static_cast<std::int64_t>(out_h) * out_w;
      const std::int64_t pix =
          static_cast<std::int64_t>(n) * kRgbCout * plane + static_cast<std::int64_t>(oy) * out_w +
          ox;
      rgb[pix]                  = rgb_v[0];
      rgb[pix + plane]          = rgb_v[1];
      rgb[pix + 2 * plane]      = rgb_v[2];
    }
  }
}

void ValidateCatAndParams(const DeviceTensor& cat, const FusedPostOutputParams& params,
                          const int out_h, const int out_w, const char* what) {
  if (cat.rank != 4 || cat.data == nullptr || !cat.IsContiguous()) {
    throw std::runtime_error(std::string(what) + ": cat must be contiguous NCHW");
  }
  if (cat.shape[1] != kPostCin) {
    throw std::runtime_error(std::string(what) + ": cat must have 6 channels");
  }
  if (params.post_channels != 24 && params.post_channels != 32) {
    throw std::runtime_error(std::string(what) + ": post_channels must be 24 or 32");
  }
  if (params.post_weight == nullptr || params.post_bias == nullptr ||
      params.output_weight_cio == nullptr || params.output_bias == nullptr) {
    throw std::runtime_error(std::string(what) + ": null weight/bias pointer");
  }
  const int H = static_cast<int>(cat.shape[2]);
  const int W = static_cast<int>(cat.shape[3]);
  if (H < 3 || W < 3) {
    throw std::runtime_error(std::string(what) + ": cat spatial too small for 3x3");
  }
  const int natural_h = H - 2;
  const int natural_w = W - 2;
  if (out_h < 1 || out_w < 1 || out_h > natural_h || out_w > natural_w) {
    throw std::runtime_error(std::string(what) + ": invalid output spatial size");
  }
  if (((natural_h - out_h) & 1) != 0 || ((natural_w - out_w) & 1) != 0) {
    throw std::runtime_error(std::string(what) + ": non-centered crop (parity)");
  }
}

void LaunchFused(const DeviceTensor& cat, float* rgb, const int out_h, const int out_w,
                 const std::size_t hwc_step_floats, const int write_hwc,
                 const FusedPostOutputParams& params, const cudaStream_t stream) {
  ValidateCatAndParams(cat, params, out_h, out_w, "FusedPostOutput");

  const int N         = static_cast<int>(cat.shape[0]);
  const int H         = static_cast<int>(cat.shape[2]);
  const int W         = static_cast<int>(cat.shape[3]);
  const int natural_h = H - 2;
  const int natural_w = W - 2;
  const int crop_y    = (natural_h - out_h) / 2;
  const int crop_x    = (natural_w - out_w) / 2;
  const int apply_g   = params.apply_gamma_decode ? 1 : 0;

  constexpr int kInH    = kOhTile + 2;
  constexpr int kInW    = kOwTile + 2;
  const int     smem_24 = static_cast<int>((kPostCin * kInH * kInW + 24 * kPostCin * 9) *
                                       sizeof(float));
  const int     smem_32 = static_cast<int>((kPostCin * kInH * kInW + 32 * kPostCin * 9) *
                                       sizeof(float));

  dim3 block(kOwTile, kOhTile);
  dim3 grid((out_w + kOwTile - 1) / kOwTile, (out_h + kOhTile - 1) / kOhTile,
            static_cast<unsigned>(N));
  if (grid.x == 0 || grid.y == 0 || grid.z == 0) {
    return;
  }

  if (params.post_channels == 24) {
    FusedPostOutputKernel<24><<<grid, block, smem_24, stream>>>(
        cat.data, params.post_weight, params.post_bias, params.output_weight_cio, params.output_bias,
        rgb, N, H, W, out_h, out_w, crop_y, crop_x, write_hwc, hwc_step_floats, apply_g);
  } else {
    FusedPostOutputKernel<32><<<grid, block, smem_32, stream>>>(
        cat.data, params.post_weight, params.post_bias, params.output_weight_cio, params.output_bias,
        rgb, N, H, W, out_h, out_w, crop_y, crop_x, write_hwc, hwc_step_floats, apply_g);
  }
  CheckCuda(cudaGetLastError(), "FusedPostOutputKernel launch");
}

}  // namespace

void PrepackOutputWeightsCio(const float* src_oihw, const int width, float* dst) {
  if (src_oihw == nullptr || dst == nullptr || width < 1) {
    throw std::runtime_error("PrepackOutputWeightsCio: invalid argument");
  }
  // src OIHW [3, width, 1, 1] → dst [width, 3]
  for (int co = 0; co < width; ++co) {
    for (int c = 0; c < kRgbCout; ++c) {
      dst[static_cast<std::size_t>(co) * kRgbCout + static_cast<std::size_t>(c)] =
          src_oihw[static_cast<std::size_t>(c) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(co)];
    }
  }
}

void FusedPostOutputToNchw(const DeviceTensor& cat, DeviceTensor& rgb,
                           const FusedPostOutputParams& params, const cudaStream_t stream) {
  if (rgb.rank != 4 || rgb.data == nullptr || !rgb.IsContiguous()) {
    throw std::runtime_error("FusedPostOutputToNchw: rgb must be contiguous NCHW");
  }
  if (rgb.shape[0] != cat.shape[0] || rgb.shape[1] != kRgbCout) {
    throw std::runtime_error("FusedPostOutputToNchw: rgb shape must be [N,3,H,W]");
  }
  LaunchFused(cat, rgb.data, static_cast<int>(rgb.shape[2]), static_cast<int>(rgb.shape[3]),
              /*hwc_step_floats=*/0, /*write_hwc=*/0, params, stream);
}

void FusedPostOutputToHwc(const DeviceTensor& cat, float* rgb_hwc, const std::size_t step_bytes,
                          const int out_h, const int out_w, const FusedPostOutputParams& params,
                          const cudaStream_t stream) {
  if (rgb_hwc == nullptr) {
    throw std::runtime_error("FusedPostOutputToHwc: null rgb pointer");
  }
  if (cat.shape[0] != 1) {
    throw std::runtime_error("FusedPostOutputToHwc: only N=1 is supported");
  }
  if (step_bytes < static_cast<std::size_t>(out_w) * kRgbCout * sizeof(float) ||
      (step_bytes % sizeof(float)) != 0) {
    throw std::runtime_error("FusedPostOutputToHwc: invalid row pitch");
  }
  LaunchFused(cat, rgb_hwc, out_h, out_w, step_bytes / sizeof(float), /*write_hwc=*/1, params,
              stream);
}

auto FusedPostOutputEnabled() -> bool {
  static const bool enabled = [] {
    const char* value = std::getenv("ALCEDO_DEMOASICNET_DISABLE_FUSED_TAIL");
    if (value != nullptr && value[0] != '\0' && value[0] != '0') {
      return false;
    }
    return true;
  }();
  return enabled;
}

}  // namespace alcedo::cuda::nn
