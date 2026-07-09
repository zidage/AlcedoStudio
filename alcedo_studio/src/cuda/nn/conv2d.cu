//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/conv2d.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

// Unified epilogue: optional bias add, optional ReLU.
__device__ __forceinline__ auto ApplyBiasRelu(float acc, const float* __restrict__ bias, int co,
                                             bool add_bias, bool do_relu) -> float {
  if (add_bias) {
    acc += bias[co];
  }
  if (do_relu) {
    acc = fmaxf(acc, 0.0f);
  }
  return acc;
}

// ---------------------------------------------------------------------------
// Generic direct NCHW convolution (groups=1). One thread per output element.
// Correct for arbitrary k/s/pad/dilation within int range; used as fallback.
// ---------------------------------------------------------------------------
__global__ void Conv2dDirectKernel(const float* __restrict__ input,
                                   const float* __restrict__ weight,
                                   const float* __restrict__ bias, float* __restrict__ output,
                                   int N, int Cin, int Cout, int H, int W, int Ho, int Wo, int kH,
                                   int kW, int sH, int sW, int padH, int padW, int dil, bool add_bias,
                                   bool do_relu) {
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(Cout) *
                             static_cast<std::int64_t>(Ho) * static_cast<std::int64_t>(Wo);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  const std::int64_t w_stride_co  = static_cast<std::int64_t>(Cin) * kH * kW;
  const std::int64_t w_stride_ci  = static_cast<std::int64_t>(kH) * kW;

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    ow  = static_cast<int>(rem % Wo);
    rem /= Wo;
    const int oh = static_cast<int>(rem % Ho);
    rem /= Ho;
    const int co = static_cast<int>(rem % Cout);
    const int n  = static_cast<int>(rem / Cout);

    float acc = 0.0f;
    const float* w_co = weight + static_cast<std::int64_t>(co) * w_stride_co;
    const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

    for (int ci = 0; ci < Cin; ++ci) {
      const float* in_c = in_n + static_cast<std::int64_t>(ci) * in_stride_c;
      const float* w_ci = w_co + static_cast<std::int64_t>(ci) * w_stride_ci;
      for (int kh = 0; kh < kH; ++kh) {
        const int ih = oh * sH - padH + kh * dil;
        if (ih < 0 || ih >= H) {
          continue;
        }
        for (int kw = 0; kw < kW; ++kw) {
          const int iw = ow * sW - padW + kw * dil;
          if (iw < 0 || iw >= W) {
            continue;
          }
          acc += in_c[static_cast<std::int64_t>(ih) * W + iw] * w_ci[kh * kW + kw];
        }
      }
    }

    acc = ApplyBiasRelu(acc, bias, co, add_bias, do_relu);
    output[static_cast<std::int64_t>(n) * out_stride_n +
           static_cast<std::int64_t>(co) * out_stride_c +
           static_cast<std::int64_t>(oh) * Wo + ow] = acc;
  }
}

// ---------------------------------------------------------------------------
// 1×1 s=1 pad=0: GEMM-style without cuBLAS.
// Each block handles a tile of spatial positions × a tile of output channels.
// Input channel reduction is done in registers with shared-memory weight tiles.
// ---------------------------------------------------------------------------
template <int kTileSpatial, int kTileCout, int kTileCin>
__global__ void Conv2d1x1TiledKernel(const float* __restrict__ input,
                                     const float* __restrict__ weight,
                                     const float* __restrict__ bias, float* __restrict__ output,
                                     int N, int Cin, int Cout, int H, int W, bool add_bias,
                                     bool do_relu) {
  // grid: (spatial_tiles, cout_tiles, N)
  const int spatial = H * W;
  const int spat0   = static_cast<int>(blockIdx.x) * kTileSpatial;
  const int co0     = static_cast<int>(blockIdx.y) * kTileCout;
  const int n       = static_cast<int>(blockIdx.z);

  // Thread maps to one (spatial_local, cout_local) within the tile.
  // blockDim.x is always kTileSpatial * kTileCout, so co_local < kTileCout.
  const int tid        = static_cast<int>(threadIdx.x);
  const int spat_local = tid % kTileSpatial;
  const int co_local   = tid / kTileSpatial;

  const int  spat  = spat0 + spat_local;
  const int  co    = co0 + co_local;
  const bool valid = (spat < spatial) && (co < Cout);

  __shared__ float w_tile[kTileCout][kTileCin];
  // Stage a strip of input for the spatial tile (one cin strip at a time).
  __shared__ float in_tile[kTileSpatial][kTileCin];

  float acc = 0.0f;

  const std::int64_t in_base =
      static_cast<std::int64_t>(n) * static_cast<std::int64_t>(Cin) * spatial;
  const std::int64_t out_base =
      static_cast<std::int64_t>(n) * static_cast<std::int64_t>(Cout) * spatial;

  for (int ci0 = 0; ci0 < Cin; ci0 += kTileCin) {
    // Cooperative load of weight tile [kTileCout, kTileCin] and input tile.
    // weight is OIHW with kH=kW=1 → [Cout, Cin]
    for (int load = tid; load < kTileCout * kTileCin; load += kTileSpatial * kTileCout) {
      const int lco = load / kTileCin;
      const int lci = load % kTileCin;
      const int gco = co0 + lco;
      const int gci = ci0 + lci;
      float     v   = 0.0f;
      if (gco < Cout && gci < Cin) {
        v = weight[static_cast<std::int64_t>(gco) * Cin + gci];
      }
      w_tile[lco][lci] = v;
    }
    for (int load = tid; load < kTileSpatial * kTileCin; load += kTileSpatial * kTileCout) {
      const int ls  = load / kTileCin;
      const int lci = load % kTileCin;
      const int gs  = spat0 + ls;
      const int gci = ci0 + lci;
      float     v   = 0.0f;
      if (gs < spatial && gci < Cin) {
        v = input[in_base + static_cast<std::int64_t>(gci) * spatial + gs];
      }
      in_tile[ls][lci] = v;
    }
    __syncthreads();

    if (valid) {
      const int cin_end = (ci0 + kTileCin <= Cin) ? kTileCin : (Cin - ci0);
#pragma unroll
      for (int lci = 0; lci < kTileCin; ++lci) {
        if (lci < cin_end) {
          acc += in_tile[spat_local][lci] * w_tile[co_local][lci];
        }
      }
    }
    __syncthreads();
  }

  if (valid) {
    acc = ApplyBiasRelu(acc, bias, co, add_bias, do_relu);
    output[out_base + static_cast<std::int64_t>(co) * spatial + spat] = acc;
  }
}

// ---------------------------------------------------------------------------
// 2×2 stride-2 pad=0 (pack_mosaick): specialized direct kernel.
// ---------------------------------------------------------------------------
__global__ void Conv2d2x2s2Kernel(const float* __restrict__ input, const float* __restrict__ weight,
                                  const float* __restrict__ bias, float* __restrict__ output, int N,
                                  int Cin, int Cout, int H, int W, int Ho, int Wo, bool add_bias,
                                  bool do_relu) {
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(Cout) *
                             static_cast<std::int64_t>(Ho) * static_cast<std::int64_t>(Wo);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  const std::int64_t w_stride_co  = static_cast<std::int64_t>(Cin) * 4;

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    ow  = static_cast<int>(rem % Wo);
    rem /= Wo;
    const int oh = static_cast<int>(rem % Ho);
    rem /= Ho;
    const int co = static_cast<int>(rem % Cout);
    const int n  = static_cast<int>(rem / Cout);

    const int ih0 = oh * 2;
    const int iw0 = ow * 2;

    float        acc  = 0.0f;
    const float* w_co = weight + static_cast<std::int64_t>(co) * w_stride_co;
    const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

    for (int ci = 0; ci < Cin; ++ci) {
      const float* in_c = in_n + static_cast<std::int64_t>(ci) * in_stride_c;
      const float* w_ci = w_co + static_cast<std::int64_t>(ci) * 4;
      // Bounds: for valid 2×2 s=2, ih0+1 < H and iw0+1 < W when Ho/Wo computed correctly.
      const float v00 = in_c[static_cast<std::int64_t>(ih0) * W + iw0];
      const float v01 = in_c[static_cast<std::int64_t>(ih0) * W + (iw0 + 1)];
      const float v10 = in_c[static_cast<std::int64_t>(ih0 + 1) * W + iw0];
      const float v11 = in_c[static_cast<std::int64_t>(ih0 + 1) * W + (iw0 + 1)];
      acc += v00 * w_ci[0] + v01 * w_ci[1] + v10 * w_ci[2] + v11 * w_ci[3];
    }

    acc = ApplyBiasRelu(acc, bias, co, add_bias, do_relu);
    output[static_cast<std::int64_t>(n) * out_stride_n +
           static_cast<std::int64_t>(co) * out_stride_c +
           static_cast<std::int64_t>(oh) * Wo + ow] = acc;
  }
}

// ---------------------------------------------------------------------------
// 3×3 s=1 pad=0: primary demosaicnet hot path.
// Grid: (ceil(Wo/TX), ceil(Ho/TY), N*Cout). Each thread one output pixel.
// Filter weights for the assigned co are cached in shared memory (Cin*9 floats).
// ---------------------------------------------------------------------------
template <int kTx, int kTy>
__global__ void Conv2d3x3s1Kernel(const float* __restrict__ input, const float* __restrict__ weight,
                                  const float* __restrict__ bias, float* __restrict__ output, int N,
                                  int Cin, int Cout, int H, int W, int Ho, int Wo, bool add_bias,
                                  bool do_relu) {
  const int ow = static_cast<int>(blockIdx.x) * kTx + static_cast<int>(threadIdx.x);
  const int oh = static_cast<int>(blockIdx.y) * kTy + static_cast<int>(threadIdx.y);
  const int bc = static_cast<int>(blockIdx.z);
  const int n  = bc / Cout;
  const int co = bc % Cout;

  // Shared filter: Cin * 9. For Cin up to 128 this is 4.5 KB — fine.
  // Use dynamic shared memory sized by launch.
  extern __shared__ float smem[];
  float*                  w_s = smem;  // [Cin * 9]

  const int tid    = static_cast<int>(threadIdx.y) * kTx + static_cast<int>(threadIdx.x);
  const int nthreads = kTx * kTy;
  const int w_elems  = Cin * 9;
  for (int i = tid; i < w_elems; i += nthreads) {
    w_s[i] = weight[static_cast<std::int64_t>(co) * w_elems + i];
  }
  __syncthreads();

  if (ow >= Wo || oh >= Ho || n >= N) {
    return;
  }

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;

  const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;
  float        acc  = 0.0f;

  // Valid 3×3: output (oh,ow) reads input [oh..oh+2, ow..ow+2]
  for (int ci = 0; ci < Cin; ++ci) {
    const float* in_c = in_n + static_cast<std::int64_t>(ci) * in_stride_c;
    const float* w_ci = w_s + ci * 9;
    const int    ih0  = oh;
    const int    iw0  = ow;
#pragma unroll
    for (int kh = 0; kh < 3; ++kh) {
      const float* row = in_c + static_cast<std::int64_t>(ih0 + kh) * W + iw0;
#pragma unroll
      for (int kw = 0; kw < 3; ++kw) {
        acc += row[kw] * w_ci[kh * 3 + kw];
      }
    }
  }

  acc = ApplyBiasRelu(acc, bias, co, add_bias, do_relu);
  output[static_cast<std::int64_t>(n) * out_stride_n + static_cast<std::int64_t>(co) * out_stride_c +
         static_cast<std::int64_t>(oh) * Wo + ow] = acc;
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
                    const Conv2dParams& params, const char* what) {
  ValidateContiguousNchw(input, what);
  ValidateContiguousNchw(output, what);

  if (params.in_channels <= 0 || params.out_channels <= 0) {
    throw std::runtime_error(std::string(what) + ": invalid channel counts");
  }
  if (params.kH < 1 || params.kW < 1 || params.sH < 1 || params.sW < 1) {
    throw std::runtime_error(std::string(what) + ": invalid kernel/stride");
  }
  if (params.padH < 0 || params.padW < 0) {
    throw std::runtime_error(std::string(what) + ": negative padding");
  }
  if (params.dilation < 1) {
    throw std::runtime_error(std::string(what) + ": dilation must be >= 1");
  }
  if (params.groups != 1) {
    throw std::runtime_error(std::string(what) +
                             ": groups != 1 not supported in Phase 2 (use Phase 3 for grouped "
                             "ConvTranspose)");
  }
  if (params.weight == nullptr) {
    throw std::runtime_error(std::string(what) + ": null weight pointer");
  }

  const int N   = static_cast<int>(input.shape[0]);
  const int Cin = static_cast<int>(input.shape[1]);
  const int H   = static_cast<int>(input.shape[2]);
  const int W   = static_cast<int>(input.shape[3]);

  if (Cin != params.in_channels) {
    throw std::runtime_error(std::string(what) + ": input channels mismatch params.in_channels");
  }
  if (N != static_cast<int>(output.shape[0])) {
    throw std::runtime_error(std::string(what) + ": batch mismatch between input and output");
  }
  if (static_cast<int>(output.shape[1]) != params.out_channels) {
    throw std::runtime_error(std::string(what) + ": output channels mismatch params.out_channels");
  }

  const int Ho = Conv2dOutputHeight(H, params);
  const int Wo = Conv2dOutputWidth(W, params);
  if (Ho <= 0 || Wo <= 0) {
    throw std::runtime_error(std::string(what) + ": non-positive output spatial size");
  }
  if (static_cast<int>(output.shape[2]) != Ho || static_cast<int>(output.shape[3]) != Wo) {
    throw std::runtime_error(std::string(what) + ": output spatial size mismatch (expected " +
                             std::to_string(Ho) + "x" + std::to_string(Wo) + ", got " +
                             std::to_string(output.shape[2]) + "x" +
                             std::to_string(output.shape[3]) + ")");
  }
}

void LaunchConv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                  bool do_relu, cudaStream_t stream, WorkspacePool* workspace) {
  // Workspace is accepted for API stability. Direct kernels need no scratch and
  // never call cudaMalloc — satisfying the "no malloc when workspace provided" exit.
  (void)workspace;

  ValidateParams(input, output, params, do_relu ? "Conv2dBiasRelu" : "Conv2d");

  const int N    = static_cast<int>(input.shape[0]);
  const int Cin  = static_cast<int>(input.shape[1]);
  const int H    = static_cast<int>(input.shape[2]);
  const int W    = static_cast<int>(input.shape[3]);
  const int Cout = params.out_channels;
  const int Ho   = static_cast<int>(output.shape[2]);
  const int Wo   = static_cast<int>(output.shape[3]);

  const bool add_bias = params.bias != nullptr;
  const float* in_ptr  = input.data;
  const float* w_ptr   = params.weight;
  const float* b_ptr   = params.bias;
  float*       out_ptr = output.data;

  const bool is_1x1 = (params.kH == 1 && params.kW == 1 && params.sH == 1 && params.sW == 1 &&
                       params.padH == 0 && params.padW == 0 && params.dilation == 1);
  const bool is_2x2s2 = (params.kH == 2 && params.kW == 2 && params.sH == 2 && params.sW == 2 &&
                         params.padH == 0 && params.padW == 0 && params.dilation == 1);
  const bool is_3x3s1 = (params.kH == 3 && params.kW == 3 && params.sH == 1 && params.sW == 1 &&
                         params.padH == 0 && params.padW == 0 && params.dilation == 1);

  if (is_1x1) {
    // Tile: 32 spatial × 8 cout → 256 threads/block
    constexpr int kTileSpatial = 32;
    constexpr int kTileCout    = 8;
    constexpr int kTileCin     = 16;
    const int spatial = H * W;
    dim3 block(kTileSpatial * kTileCout);
    dim3 grid((spatial + kTileSpatial - 1) / kTileSpatial, (Cout + kTileCout - 1) / kTileCout, N);
    if (grid.x > 0 && grid.y > 0 && grid.z > 0) {
      Conv2d1x1TiledKernel<kTileSpatial, kTileCout, kTileCin>
          <<<grid, block, 0, stream>>>(in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, add_bias,
                                       do_relu);
      CheckCuda(cudaGetLastError(), "Conv2d1x1TiledKernel launch");
    }
    return;
  }

  if (is_2x2s2) {
    const std::int64_t numel = static_cast<std::int64_t>(N) * Cout * Ho * Wo;
    if (numel > 0) {
      const int grid = ChooseGridSize(numel, kBlockSize);
      Conv2d2x2s2Kernel<<<grid, kBlockSize, 0, stream>>>(in_ptr, w_ptr, b_ptr, out_ptr, N, Cin,
                                                         Cout, H, W, Ho, Wo, add_bias, do_relu);
      CheckCuda(cudaGetLastError(), "Conv2d2x2s2Kernel launch");
    }
    return;
  }

  if (is_3x3s1) {
    constexpr int kTx = 16;
    constexpr int kTy = 8;
    dim3          block(kTx, kTy);
    dim3          grid((Wo + kTx - 1) / kTx, (Ho + kTy - 1) / kTy, N * Cout);
    // Cap grid.z if huge; N*Cout for demosaicnet is at most ~128.
    if (grid.x > 0 && grid.y > 0 && grid.z > 0) {
      const std::size_t smem = static_cast<std::size_t>(Cin) * 9U * sizeof(float);
      Conv2d3x3s1Kernel<kTx, kTy><<<grid, block, smem, stream>>>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu);
      CheckCuda(cudaGetLastError(), "Conv2d3x3s1Kernel launch");
    }
    return;
  }

  // Generic fallback (covers any remaining k/s/pad/dilation with groups=1).
  const std::int64_t numel = static_cast<std::int64_t>(N) * Cout * Ho * Wo;
  if (numel > 0) {
    const int grid = ChooseGridSize(numel, kBlockSize);
    Conv2dDirectKernel<<<grid, kBlockSize, 0, stream>>>(
        in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, params.kH, params.kW, params.sH,
        params.sW, params.padH, params.padW, params.dilation, add_bias, do_relu);
    CheckCuda(cudaGetLastError(), "Conv2dDirectKernel launch");
  }
}

}  // namespace

void Conv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
            cudaStream_t stream, WorkspacePool* workspace) {
  LaunchConv2d(input, output, params, /*do_relu=*/false, stream, workspace);
}

void Conv2dBiasRelu(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                    cudaStream_t stream, WorkspacePool* workspace) {
  LaunchConv2d(input, output, params, /*do_relu=*/true, stream, workspace);
}

}  // namespace alcedo::cuda::nn
