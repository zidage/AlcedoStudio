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

    float        acc  = 0.0f;
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
// 3×3 s=1 pad=0 — demosaicnet hot path (major overhaul).
//
// Prior kernel (one Cout per block, weights-only SMEM):
//   - Reloaded the same input spatial tile from global memory Cout times.
//   - Each thread reduced Cin×9 with no cross-channel input reuse.
//   - Measured ~0.4–0.5 s per 64→64 layer at ~1024 tile (tens of GFLOP/s).
//
// New design: implicit multi-Cout direct convolution (hand-written, no cuBLAS
// / cuDNN), matching the industry pattern used by CUTLASS-style direct /
// implicit-GEMM CNN kernels:
//
//   Block owns  OH_TILE × OW_TILE output pixels × COUT_TILE output channels.
//   Cin is reduced in CIN_TILE strips.
//   Shared memory holds:
//     - input apron (OH_TILE+2) × (OW_TILE+2) × CIN_TILE
//     - weight slice COUT_TILE × CIN_TILE × 9  (OIHW slice)
//   Each thread owns one output pixel and accumulates COUT_TILE values in
//   registers. The 3×3 input window is loaded once per (pixel, cin) and reused
//   across all COUT_TILE output channels → arithmetic intensity ×COUT_TILE.
//
// Grid: (ceil(Wo/OW), ceil(Ho/OH), N * ceil(Cout/COUT_TILE))
// Block: (OW_TILE, OH_TILE)
// ---------------------------------------------------------------------------
template <int kOhTile, int kOwTile, int kCoutTile, int kCinTile>
__global__ void Conv2d3x3s1TiledKernel(const float* __restrict__ input,
                                       const float* __restrict__ weight,
                                       const float* __restrict__ bias, float* __restrict__ output,
                                       int N, int Cin, int Cout, int H, int W, int Ho, int Wo,
                                       bool add_bias, bool do_relu) {
  constexpr int kInH    = kOhTile + 2;
  constexpr int kInW    = kOwTile + 2;
  constexpr int kInArea = kInH * kInW;
  constexpr int kWElems = kCoutTile * kCinTile * 9;

  const int ow0 = static_cast<int>(blockIdx.x) * kOwTile;
  const int oh0 = static_cast<int>(blockIdx.y) * kOhTile;

  const int cout_tiles = (Cout + kCoutTile - 1) / kCoutTile;
  const int bc         = static_cast<int>(blockIdx.z);
  const int n          = bc / cout_tiles;
  const int co0        = (bc % cout_tiles) * kCoutTile;

  const int tx = static_cast<int>(threadIdx.x);
  const int ty = static_cast<int>(threadIdx.y);
  const int tid =
      ty * kOwTile + tx;  // 0 .. kOhTile*kOwTile-1
  constexpr int kThreads = kOhTile * kOwTile;

  // Dynamic SMEM: [input apron | weights]
  // Layout input: [CIN_TILE][IN_H][IN_W]  — consecutive W for coalesced loads
  // Layout weight: [COUT_TILE][CIN_TILE][9]
  extern __shared__ float smem[];
  float*                  in_s = smem;                    // kCinTile * kInArea
  float*                  w_s  = smem + kCinTile * kInArea;  // kWElems

  float acc[kCoutTile];
#pragma unroll
  for (int i = 0; i < kCoutTile; ++i) {
    acc[i] = 0.0f;
  }

  const int oh = oh0 + ty;
  const int ow = ow0 + tx;
  const bool pixel_valid = (n < N) && (oh < Ho) && (ow < Wo);

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  const std::int64_t w_stride_co  = static_cast<std::int64_t>(Cin) * 9;

  const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

  for (int ci0 = 0; ci0 < Cin; ci0 += kCinTile) {
    const int cin_tile = (ci0 + kCinTile <= Cin) ? kCinTile : (Cin - ci0);

    // ---- Cooperative load: input apron for this cin strip ----
    // Total elements = kCinTile * kInArea (pad unused cin slots with 0).
    for (int load = tid; load < kCinTile * kInArea; load += kThreads) {
      const int lci = load / kInArea;
      const int rem = load - lci * kInArea;
      const int lh  = rem / kInW;
      const int lw  = rem - lh * kInW;

      const int gci = ci0 + lci;
      const int gh  = oh0 + lh;
      const int gw  = ow0 + lw;

      float v = 0.0f;
      // Valid 3×3: interior of a full tile is always in-bounds. Edge partial
      // tiles and cin padding need the guard.
      if (lci < cin_tile && gci < Cin && gh >= 0 && gh < H && gw >= 0 && gw < W) {
        v = in_n[static_cast<std::int64_t>(gci) * in_stride_c +
                 static_cast<std::int64_t>(gh) * W + gw];
      }
      in_s[load] = v;
    }

    // ---- Cooperative load: weight slice [COUT_TILE, CIN_TILE, 9] ----
    for (int load = tid; load < kWElems; load += kThreads) {
      // linear: ((lco * kCinTile) + lci) * 9 + k
      const int k   = load % 9;
      const int tmp = load / 9;
      const int lci = tmp % kCinTile;
      const int lco = tmp / kCinTile;

      const int gco = co0 + lco;
      const int gci = ci0 + lci;

      float v = 0.0f;
      if (lco < kCoutTile && gco < Cout && lci < cin_tile && gci < Cin) {
        v = weight[static_cast<std::int64_t>(gco) * w_stride_co +
                   static_cast<std::int64_t>(gci) * 9 + k];
      }
      w_s[load] = v;
    }
    __syncthreads();

    // ---- Compute: reuse 3×3 window across COUT_TILE channels ----
    // Avoid `break` inside #pragma unroll (nvcc codegen is unreliable).
    if (pixel_valid) {
#pragma unroll
      for (int lci = 0; lci < kCinTile; ++lci) {
        if (lci < cin_tile) {
          // Apron origin for this pixel is (ty, tx) in [0..OH+1] × [0..OW+1].
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
          for (int lco = 0; lco < kCoutTile; ++lco) {
            const float* w = w_s + (lco * kCinTile + lci) * 9;
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
      }
    }
    __syncthreads();
  }

  // ---- Store epilogue ----
  if (pixel_valid) {
#pragma unroll
    for (int lco = 0; lco < kCoutTile; ++lco) {
      const int co = co0 + lco;
      if (co < Cout) {
        const float v = ApplyBiasRelu(acc[lco], bias, co, add_bias, do_relu);
        output[static_cast<std::int64_t>(n) * out_stride_n +
               static_cast<std::int64_t>(co) * out_stride_c +
               static_cast<std::int64_t>(oh) * Wo + ow] = v;
      }
    }
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

// Launch the multi-Cout 3×3 tiled kernel with a fixed tile configuration.
// Tile sizes are chosen for demosaicnet (C≈64, large spatial, pad=0 valid):
//   OH=8, OW=16 → 128 threads/block
//   COUT=16      → 16 register accumulators / thread (good occupancy)
//   CIN=8        → modest SMEM, frequent reuse of input apron
// SMEM ≈ 8*10*18*4 + 16*8*9*4 ≈ 10.3 KiB → multiple blocks/SM.
template <int kOhTile, int kOwTile, int kCoutTile, int kCinTile>
void LaunchConv2d3x3Tiled(const float* in_ptr, const float* w_ptr, const float* b_ptr,
                          float* out_ptr, int N, int Cin, int Cout, int H, int W, int Ho, int Wo,
                          bool add_bias, bool do_relu, cudaStream_t stream) {
  constexpr int kInH    = kOhTile + 2;
  constexpr int kInW    = kOwTile + 2;
  constexpr int kSmemBytes =
      static_cast<int>((kCinTile * kInH * kInW + kCoutTile * kCinTile * 9) * sizeof(float));

  const int cout_tiles = (Cout + kCoutTile - 1) / kCoutTile;
  dim3      block(kOwTile, kOhTile);
  dim3      grid((Wo + kOwTile - 1) / kOwTile, (Ho + kOhTile - 1) / kOhTile,
                 static_cast<unsigned>(N) * static_cast<unsigned>(cout_tiles));

  if (grid.x == 0 || grid.y == 0 || grid.z == 0) {
    return;
  }

  Conv2d3x3s1TiledKernel<kOhTile, kOwTile, kCoutTile, kCinTile>
      <<<grid, block, kSmemBytes, stream>>>(in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho,
                                            Wo, add_bias, do_relu);
  CheckCuda(cudaGetLastError(), "Conv2d3x3s1TiledKernel launch");
}

void LaunchConv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                  bool do_relu, cudaStream_t stream, WorkspacePool* workspace) {
  // Workspace is accepted for API stability. Current kernels need no scratch and
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

  const bool   add_bias = params.bias != nullptr;
  const float* in_ptr   = input.data;
  const float* w_ptr    = params.weight;
  const float* b_ptr    = params.bias;
  float*       out_ptr  = output.data;

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
    const int     spatial      = H * W;
    dim3          block(kTileSpatial * kTileCout);
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
    // Multi-Cout tiled direct conv. Dispatch by Cout tile so student shapes fill
    // blocks exactly: bayer_s24_d8 (Cout=24), xtrans_p2_s32_d4 (Cout=32), and
    // residual teacher-sized layers (Cout>=32). Cin is always reduced in kCinTile
    // strips — do not gate the Cout tile on Cin (post_conv is 6→24/32).
    if (Cout >= 32) {
      // X-Trans trunk 32→32 / post 6→32, and any wider nets.
      LaunchConv2d3x3Tiled</*OH=*/8, /*OW=*/16, /*Cout=*/32, /*Cin=*/8>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu, stream);
    } else if (Cout >= 24) {
      // Bayer student trunk 24→24 and post 6→24: one exact Cout tile (was 16+partial).
      LaunchConv2d3x3Tiled</*OH=*/8, /*OW=*/16, /*Cout=*/24, /*Cin=*/8>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu, stream);
    } else if (Cout >= 16) {
      LaunchConv2d3x3Tiled</*OH=*/8, /*OW=*/16, /*Cout=*/16, /*Cin=*/8>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu, stream);
    } else {
      LaunchConv2d3x3Tiled</*OH=*/8, /*OW=*/16, /*Cout=*/8, /*Cin=*/8>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu, stream);
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
