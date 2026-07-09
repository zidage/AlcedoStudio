//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/conv_transpose2d.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

// ---------------------------------------------------------------------------
// Specialized unpack_mosaick: Cin=12, Cout=3, k=2, s=2, pad=0, groups=3, dil=1,
// output_padding=0. Spatial out is exactly 2× input.
//
// Per group: 4 input channels → 1 output channel. Weight [12, 1, 2, 2].
// For s=k=2 pad=0 each output pixel gathers from exactly one input site with a
// single kernel offset (kh=oh%2, kw=ow%2, ih=oh/2, iw=ow/2).
// ---------------------------------------------------------------------------
__global__ void ConvTransposeUnpackMosaickKernel(const float* __restrict__ input,
                                                 const float* __restrict__ weight,
                                                 const float* __restrict__ bias,
                                                 float* __restrict__ output, int N, int H, int W,
                                                 int Ho, int Wo, bool add_bias) {
  // Fixed: Cin=12, Cout=3, groups=3 → 4 in / 1 out per group.
  constexpr int kCinPerGroup = 4;
  constexpr int kCout        = 3;

  const std::int64_t numel = static_cast<std::int64_t>(N) * kCout *
                             static_cast<std::int64_t>(Ho) * static_cast<std::int64_t>(Wo);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  const std::int64_t in_stride_n  = 12LL * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(kCout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  // weight [Cin, 1, 2, 2] → 4 floats per input channel
  const std::int64_t w_stride_ci  = 4;

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    ow  = static_cast<int>(rem % Wo);
    rem /= Wo;
    const int oh = static_cast<int>(rem % Ho);
    rem /= Ho;
    const int co = static_cast<int>(rem % kCout);
    const int n  = static_cast<int>(rem / kCout);

    const int kh = oh & 1;
    const int kw = ow & 1;
    const int ih = oh >> 1;
    const int iw = ow >> 1;

    // Safety: for Ho=2*H, Wo=2*W this always holds; keep for odd/defensive sizes.
    if (ih < 0 || ih >= H || iw < 0 || iw >= W) {
      float acc = 0.0f;
      if (add_bias) {
        acc += bias[co];
      }
      output[static_cast<std::int64_t>(n) * out_stride_n +
             static_cast<std::int64_t>(co) * out_stride_c +
             static_cast<std::int64_t>(oh) * Wo + ow] = acc;
      continue;
    }

    // co maps 1:1 to group (cout_per_group = 1).
    const int g      = co;
    const int ci0    = g * kCinPerGroup;
    const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

    float acc = 0.0f;
#pragma unroll
    for (int cin_g = 0; cin_g < kCinPerGroup; ++cin_g) {
      const int    ci   = ci0 + cin_g;
      const float  inv  = in_n[static_cast<std::int64_t>(ci) * in_stride_c +
                              static_cast<std::int64_t>(ih) * W + iw];
      // w[ci, 0, kh, kw]
      const float  wv   = weight[static_cast<std::int64_t>(ci) * w_stride_ci +
                                static_cast<std::int64_t>(kh) * 2 + kw];
      acc += inv * wv;
    }

    if (add_bias) {
      acc += bias[co];
    }
    output[static_cast<std::int64_t>(n) * out_stride_n +
           static_cast<std::int64_t>(co) * out_stride_c +
           static_cast<std::int64_t>(oh) * Wo + ow] = acc;
  }
}

// ---------------------------------------------------------------------------
// Generic grouped ConvTranspose2d (gather form). Weight [Cin, Cout/g, kH, kW].
// Correct for arbitrary k/s/pad/dilation/output_padding within int range.
// ---------------------------------------------------------------------------
__global__ void ConvTranspose2dDirectKernel(const float* __restrict__ input,
                                            const float* __restrict__ weight,
                                            const float* __restrict__ bias,
                                            float* __restrict__ output, int N, int Cin, int Cout,
                                            int H, int W, int Ho, int Wo, int kH, int kW, int sH,
                                            int sW, int padH, int padW, int dil, int groups,
                                            bool add_bias) {
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(Cout) *
                             static_cast<std::int64_t>(Ho) * static_cast<std::int64_t>(Wo);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  const int cin_per_group  = Cin / groups;
  const int cout_per_group = Cout / groups;

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  // weight [Cin, Cout/g, kH, kW]
  const std::int64_t w_stride_ci  = static_cast<std::int64_t>(cout_per_group) * kH * kW;
  const std::int64_t w_stride_co  = static_cast<std::int64_t>(kH) * kW;

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    ow  = static_cast<int>(rem % Wo);
    rem /= Wo;
    const int oh = static_cast<int>(rem % Ho);
    rem /= Ho;
    const int co = static_cast<int>(rem % Cout);
    const int n  = static_cast<int>(rem / Cout);

    const int g    = co / cout_per_group;
    const int co_g = co % cout_per_group;
    const int ci0  = g * cin_per_group;

    float        acc  = 0.0f;
    const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

    for (int cin_g = 0; cin_g < cin_per_group; ++cin_g) {
      const int    ci   = ci0 + cin_g;
      const float* in_c = in_n + static_cast<std::int64_t>(ci) * in_stride_c;
      const float* w_ci = weight + static_cast<std::int64_t>(ci) * w_stride_ci +
                         static_cast<std::int64_t>(co_g) * w_stride_co;

      for (int kh = 0; kh < kH; ++kh) {
        // oh = ih * sH - padH + kh * dil  ⇒  ih * sH = oh + padH - kh * dil
        const int num_h = oh + padH - kh * dil;
        if (num_h < 0 || (num_h % sH) != 0) {
          continue;
        }
        const int ih = num_h / sH;
        if (ih >= H) {
          continue;
        }
        for (int kw = 0; kw < kW; ++kw) {
          const int num_w = ow + padW - kw * dil;
          if (num_w < 0 || (num_w % sW) != 0) {
            continue;
          }
          const int iw = num_w / sW;
          if (iw >= W) {
            continue;
          }
          acc += in_c[static_cast<std::int64_t>(ih) * W + iw] * w_ci[kh * kW + kw];
        }
      }
    }

    if (add_bias) {
      acc += bias[co];
    }
    output[static_cast<std::int64_t>(n) * out_stride_n +
           static_cast<std::int64_t>(co) * out_stride_c +
           static_cast<std::int64_t>(oh) * Wo + ow] = acc;
  }
}

// ---------------------------------------------------------------------------
// Host-side validation + dispatch
// ---------------------------------------------------------------------------

void ValidateContiguousNchw(const DeviceTensor& t, const char* what) {
  if (t.rank != 4) {
    throw std::runtime_error(std::string(what) + ": tensor must be rank-4 NCHW");
  }
  if (t.data == nullptr) {
    throw std::runtime_error(std::string(what) + ": null data pointer");
  }
  if (!t.IsContiguous()) {
    throw std::runtime_error(std::string(what) + ": tensor must be contiguous NCHW");
  }
}

void ValidateParams(const DeviceTensor& input, const DeviceTensor& output,
                    const ConvTranspose2dParams& params) {
  constexpr const char* kWhat = "ConvTranspose2d";
  ValidateContiguousNchw(input, kWhat);
  ValidateContiguousNchw(output, kWhat);

  if (params.in_channels <= 0 || params.out_channels <= 0) {
    throw std::runtime_error(std::string(kWhat) + ": invalid channel counts");
  }
  if (params.kH < 1 || params.kW < 1 || params.sH < 1 || params.sW < 1) {
    throw std::runtime_error(std::string(kWhat) + ": invalid kernel/stride");
  }
  if (params.padH < 0 || params.padW < 0) {
    throw std::runtime_error(std::string(kWhat) + ": negative padding");
  }
  if (params.output_padH < 0 || params.output_padW < 0) {
    throw std::runtime_error(std::string(kWhat) + ": negative output_padding");
  }
  if (params.dilation < 1) {
    throw std::runtime_error(std::string(kWhat) + ": dilation must be >= 1");
  }
  if (params.groups < 1) {
    throw std::runtime_error(std::string(kWhat) + ": groups must be >= 1");
  }
  if (params.in_channels % params.groups != 0 || params.out_channels % params.groups != 0) {
    throw std::runtime_error(std::string(kWhat) +
                             ": in/out channels must be divisible by groups");
  }
  if (params.weight == nullptr) {
    throw std::runtime_error(std::string(kWhat) + ": null weight pointer");
  }

  const int N   = static_cast<int>(input.shape[0]);
  const int Cin = static_cast<int>(input.shape[1]);
  const int H   = static_cast<int>(input.shape[2]);
  const int W   = static_cast<int>(input.shape[3]);

  if (Cin != params.in_channels) {
    throw std::runtime_error(std::string(kWhat) + ": input channels mismatch params.in_channels");
  }
  if (N != static_cast<int>(output.shape[0])) {
    throw std::runtime_error(std::string(kWhat) + ": batch mismatch between input and output");
  }
  if (static_cast<int>(output.shape[1]) != params.out_channels) {
    throw std::runtime_error(std::string(kWhat) + ": output channels mismatch params.out_channels");
  }

  const int Ho = ConvTranspose2dOutputHeight(H, params);
  const int Wo = ConvTranspose2dOutputWidth(W, params);
  if (Ho <= 0 || Wo <= 0) {
    throw std::runtime_error(std::string(kWhat) + ": non-positive output spatial size");
  }
  if (static_cast<int>(output.shape[2]) != Ho || static_cast<int>(output.shape[3]) != Wo) {
    throw std::runtime_error(std::string(kWhat) + ": output spatial size mismatch (expected " +
                             std::to_string(Ho) + "x" + std::to_string(Wo) + ", got " +
                             std::to_string(output.shape[2]) + "x" +
                             std::to_string(output.shape[3]) + ")");
  }
}

void LaunchConvTranspose2d(const DeviceTensor& input, DeviceTensor& output,
                           const ConvTranspose2dParams& params, cudaStream_t stream,
                           WorkspacePool* workspace) {
  // Direct kernels need no scratch and never call cudaMalloc.
  (void)workspace;

  ValidateParams(input, output, params);

  const int N    = static_cast<int>(input.shape[0]);
  const int Cin  = static_cast<int>(input.shape[1]);
  const int H    = static_cast<int>(input.shape[2]);
  const int W    = static_cast<int>(input.shape[3]);
  const int Cout = params.out_channels;
  const int Ho   = static_cast<int>(output.shape[2]);
  const int Wo   = static_cast<int>(output.shape[3]);

  const bool   add_bias = params.bias != nullptr;
  const float* in_ptr   = input.data;
  const float* w_ptr    = params.weight;
  const float* b_ptr    = params.bias;
  float*       out_ptr  = output.data;

  // Required demosaicnet specialization: unpack_mosaick.
  const bool is_unpack =
      (Cin == 12 && Cout == 3 && params.groups == 3 && params.kH == 2 && params.kW == 2 &&
       params.sH == 2 && params.sW == 2 && params.padH == 0 && params.padW == 0 &&
       params.output_padH == 0 && params.output_padW == 0 && params.dilation == 1);

  const std::int64_t numel = static_cast<std::int64_t>(N) * Cout * Ho * Wo;
  if (numel <= 0) {
    return;
  }
  const int grid = ChooseGridSize(numel, kBlockSize);

  if (is_unpack) {
    ConvTransposeUnpackMosaickKernel<<<grid, kBlockSize, 0, stream>>>(
        in_ptr, w_ptr, b_ptr, out_ptr, N, H, W, Ho, Wo, add_bias);
    CheckCuda(cudaGetLastError(), "ConvTransposeUnpackMosaickKernel launch");
    return;
  }

  ConvTranspose2dDirectKernel<<<grid, kBlockSize, 0, stream>>>(
      in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, params.kH, params.kW, params.sH,
      params.sW, params.padH, params.padW, params.dilation, params.groups, add_bias);
  CheckCuda(cudaGetLastError(), "ConvTranspose2dDirectKernel launch");
}

}  // namespace

void ConvTranspose2d(const DeviceTensor& input, DeviceTensor& output,
                     const ConvTranspose2dParams& params, cudaStream_t stream,
                     WorkspacePool* workspace) {
  LaunchConvTranspose2d(input, output, params, stream, workspace);
}

}  // namespace alcedo::cuda::nn
