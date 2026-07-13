//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/nn/common.hpp"
#include "cuda/nn/conv2d.hpp"

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
                                   const float* __restrict__ weight, const float* __restrict__ bias,
                                   float* __restrict__ output, int N, int Cin, int Cout, int H,
                                   int W, int Ho, int Wo, int kH, int kW, int sH, int sW, int padH,
                                   int padW, int dil, bool add_bias, bool do_relu) {
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
    const int    co   = static_cast<int>(rem % Cout);
    const int    n    = static_cast<int>(rem / Cout);

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
           static_cast<std::int64_t>(co) * out_stride_c + static_cast<std::int64_t>(oh) * Wo + ow] =
        acc;
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
  const int          spatial    = H * W;
  const int          spat0      = static_cast<int>(blockIdx.x) * kTileSpatial;
  const int          co0        = static_cast<int>(blockIdx.y) * kTileCout;
  const int          n          = static_cast<int>(blockIdx.z);

  // Thread maps to one (spatial_local, cout_local) within the tile.
  // blockDim.x is always kTileSpatial * kTileCout, so co_local < kTileCout.
  const int          tid        = static_cast<int>(threadIdx.x);
  const int          spat_local = tid % kTileSpatial;
  const int          co_local   = tid / kTileSpatial;

  const int          spat       = spat0 + spat_local;
  const int          co         = co0 + co_local;
  const bool         valid      = (spat < spatial) && (co < Cout);

  __shared__ float   w_tile[kTileCout][kTileCin];
  // Stage a strip of input for the spatial tile (one cin strip at a time).
  __shared__ float   in_tile[kTileSpatial][kTileCin];

  float              acc = 0.0f;

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

// Exact small-Cout 1x1 kernel for the student residual (Cout=12) and output
// (Cout=3) layers. One thread owns one spatial position and accumulates every
// output channel in registers. Compared with the generic 8-Cout tiled kernel,
// this avoids a mostly-empty Cout tile for 3 outputs and avoids loading the
// same input spatial position twice for 12 outputs. Adjacent threads read the
// same channel plane at adjacent pixels, so NCHW input and output accesses are
// coalesced; weights are warp-uniform and served by the read-only cache.
template <int kCout>
__global__ void Conv2d1x1SmallCoutKernel(const float* __restrict__ input,
                                         const float* __restrict__ weight,
                                         const float* __restrict__ bias, float* __restrict__ output,
                                         int N, int Cin, int spatial, bool add_bias, bool do_relu) {
  static_assert(kCout == 3 || kCout == 12, "unsupported small-Cout specialization");

  const std::int64_t numel  = static_cast<std::int64_t>(N) * spatial;
  const std::int64_t stride = static_cast<std::int64_t>(blockDim.x) * gridDim.x;

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += stride) {
    const int n = static_cast<int>(linear / spatial);
    const int s = static_cast<int>(linear - static_cast<std::int64_t>(n) * spatial);

    float     acc[kCout];
#pragma unroll
    for (int co = 0; co < kCout; ++co) {
      acc[co] = 0.0F;
    }

    const float* in_n = input + static_cast<std::int64_t>(n) * Cin * spatial;
#pragma unroll 1
    for (int ci = 0; ci < Cin; ++ci) {
      const float v = in_n[static_cast<std::int64_t>(ci) * spatial + s];
#pragma unroll
      for (int co = 0; co < kCout; ++co) {
        acc[co] = fmaf(v, weight[static_cast<std::int64_t>(co) * Cin + ci], acc[co]);
      }
    }

    float* out_n = output + static_cast<std::int64_t>(n) * kCout * spatial;
#pragma unroll
    for (int co = 0; co < kCout; ++co) {
      out_n[static_cast<std::int64_t>(co) * spatial + s] =
          ApplyBiasRelu(acc[co], bias, co, add_bias, do_relu);
    }
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
    const int    co   = static_cast<int>(rem % Cout);
    const int    n    = static_cast<int>(rem / Cout);

    const int    ih0  = oh * 2;
    const int    iw0  = ow * 2;

    float        acc  = 0.0f;
    const float* w_co = weight + static_cast<std::int64_t>(co) * w_stride_co;
    const float* in_n = input + static_cast<std::int64_t>(n) * in_stride_n;

    for (int ci = 0; ci < Cin; ++ci) {
      const float* in_c = in_n + static_cast<std::int64_t>(ci) * in_stride_c;
      const float* w_ci = w_co + static_cast<std::int64_t>(ci) * 4;
      // Bounds: for valid 2×2 s=2, ih0+1 < H and iw0+1 < W when Ho/Wo computed correctly.
      const float  v00  = in_c[static_cast<std::int64_t>(ih0) * W + iw0];
      const float  v01  = in_c[static_cast<std::int64_t>(ih0) * W + (iw0 + 1)];
      const float  v10  = in_c[static_cast<std::int64_t>(ih0 + 1) * W + iw0];
      const float  v11  = in_c[static_cast<std::int64_t>(ih0 + 1) * W + (iw0 + 1)];
      acc += v00 * w_ci[0] + v01 * w_ci[1] + v10 * w_ci[2] + v11 * w_ci[3];
    }

    acc = ApplyBiasRelu(acc, bias, co, add_bias, do_relu);
    output[static_cast<std::int64_t>(n) * out_stride_n +
           static_cast<std::int64_t>(co) * out_stride_c + static_cast<std::int64_t>(oh) * Wo + ow] =
        acc;
  }
}

// ---------------------------------------------------------------------------
// 3×3 s=1 pad=0 — multi-Cout tiled direct conv.
//
// Block owns OH_TILE × OW_TILE output pixels × COUT_TILE output channels.
// Cin is reduced in CIN_TILE strips. Shared memory holds the input apron and
// an OIHW weight slice. Each thread owns one output pixel and accumulates
// COUT_TILE values in registers.
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
  constexpr int           kInH       = kOhTile + 2;
  constexpr int           kInW       = kOwTile + 2;
  constexpr int           kInArea    = kInH * kInW;
  constexpr int           kWElems    = kCoutTile * kCinTile * 9;

  const int               ow0        = static_cast<int>(blockIdx.x) * kOwTile;
  const int               oh0        = static_cast<int>(blockIdx.y) * kOhTile;

  const int               cout_tiles = (Cout + kCoutTile - 1) / kCoutTile;
  const int               bc         = static_cast<int>(blockIdx.z);
  const int               n          = bc / cout_tiles;
  const int               co0        = (bc % cout_tiles) * kCoutTile;

  const int               tx         = static_cast<int>(threadIdx.x);
  const int               ty         = static_cast<int>(threadIdx.y);
  const int               tid        = ty * kOwTile + tx;  // 0 .. kOhTile*kOwTile-1
  constexpr int           kThreads   = kOhTile * kOwTile;

  // Dynamic SMEM: [input apron | weights]
  // Layout input: [CIN_TILE][IN_H][IN_W]  — consecutive W for coalesced loads
  // Layout weight: [COUT_TILE][CIN_TILE][9]
  extern __shared__ float smem[];
  float*                  in_s = smem;                       // kCinTile * kInArea
  float*                  w_s  = smem + kCinTile * kInArea;  // kWElems

  float                   acc[kCoutTile];
#pragma unroll
  for (int i = 0; i < kCoutTile; ++i) {
    acc[i] = 0.0f;
  }

  const int          oh           = oh0 + ty;
  const int          ow           = ow0 + tx;
  const bool         pixel_valid  = (n < N) && (oh < Ho) && (ow < Wo);

  const std::int64_t in_stride_n  = static_cast<std::int64_t>(Cin) * H * W;
  const std::int64_t in_stride_c  = static_cast<std::int64_t>(H) * W;
  const std::int64_t out_stride_n = static_cast<std::int64_t>(Cout) * Ho * Wo;
  const std::int64_t out_stride_c = static_cast<std::int64_t>(Ho) * Wo;
  const std::int64_t w_stride_co  = static_cast<std::int64_t>(Cin) * 9;

  const float*       in_n         = input + static_cast<std::int64_t>(n) * in_stride_n;

  for (int ci0 = 0; ci0 < Cin; ci0 += kCinTile) {
    const int cin_tile = (ci0 + kCinTile <= Cin) ? kCinTile : (Cin - ci0);

    // ---- Cooperative load: input apron for this cin strip ----
    for (int load = tid; load < kCinTile * kInArea; load += kThreads) {
      const int lci = load / kInArea;
      const int rem = load - lci * kInArea;
      const int lh  = rem / kInW;
      const int lw  = rem - lh * kInW;

      const int gci = ci0 + lci;
      const int gh  = oh0 + lh;
      const int gw  = ow0 + lw;

      float     v   = 0.0f;
      if (lci < cin_tile && gci < Cin && gh >= 0 && gh < H && gw >= 0 && gw < W) {
        v = in_n[static_cast<std::int64_t>(gci) * in_stride_c + static_cast<std::int64_t>(gh) * W +
                 gw];
      }
      in_s[load] = v;
    }

    // ---- Cooperative load: weight slice [COUT_TILE, CIN_TILE, 9] ----
    for (int load = tid; load < kWElems; load += kThreads) {
      const int k   = load % 9;
      const int tmp = load / 9;
      const int lci = tmp % kCinTile;
      const int lco = tmp / kCinTile;

      const int gco = co0 + lco;
      const int gci = ci0 + lci;

      float     v   = 0.0f;
      if (lco < kCoutTile && gco < Cout && lci < cin_tile && gci < Cin) {
        v = weight[static_cast<std::int64_t>(gco) * w_stride_co +
                   static_cast<std::int64_t>(gci) * 9 + k];
      }
      w_s[load] = v;
    }
    __syncthreads();

    if (pixel_valid) {
#pragma unroll
      for (int lci = 0; lci < kCinTile; ++lci) {
        if (lci < cin_tile) {
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

  if (pixel_valid) {
#pragma unroll
    for (int lco = 0; lco < kCoutTile; ++lco) {
      const int co = co0 + lco;
      if (co < Cout) {
        const float v = ApplyBiasRelu(acc[lco], bias, co, add_bias, do_relu);
        output[static_cast<std::int64_t>(n) * out_stride_n +
               static_cast<std::int64_t>(co) * out_stride_c + static_cast<std::int64_t>(oh) * Wo +
               ow]    = v;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Bayer product channels-last 3×3 trunk (C=24 only).
//
// One block owns an 8x16 spatial tile and all 24 channels. The input apron and
// prepacked [Cin,3,3,Cout] weights stay channels-last in shared memory.
// Cin/Cout are multiples of four, so the cooperative global loads and final
// stores are float4 operations. X-Trans C=32 uses CUTLASS instead.
// ---------------------------------------------------------------------------
template <int kCout, int kOhTile, int kOwTile>
__global__ void Conv2d3x3s1NhwcTiledKernel(const float* __restrict__ input,
                                           const float* __restrict__ weight_ckco,
                                           const float* __restrict__ bias,
                                           float* __restrict__ output, int N, int H, int W, int Cin,
                                           int Ho, int Wo) {
  constexpr int           kInH     = kOhTile + 2;
  constexpr int           kInW     = kOwTile + 2;
  constexpr int           kInArea  = kInH * kInW;
  constexpr int           kThreads = kOhTile * kOwTile;

  const int               ox0      = static_cast<int>(blockIdx.x) * kOwTile;
  const int               oy0      = static_cast<int>(blockIdx.y) * kOhTile;
  const int               n        = static_cast<int>(blockIdx.z);
  const int               tx       = static_cast<int>(threadIdx.x);
  const int               ty       = static_cast<int>(threadIdx.y);
  const int               tid      = ty * kOwTile + tx;

  extern __shared__ float smem[];
  constexpr int           kCinTile    = 8;
  float*                  in_s        = smem;                       // [in_area][8]
  float*                  w_s         = smem + kInArea * kCinTile;  // [8][3][3][Cout]

  const int               oy          = oy0 + ty;
  const int               ox          = ox0 + tx;
  const bool              pixel_valid = (n < N) && (oy < Ho) && (ox < Wo);
  constexpr int           kCout4      = kCout / 4;

  float                   acc[kCout];
#pragma unroll
  for (int co = 0; co < kCout; ++co) {
    acc[co] = 0.0F;
  }

  for (int ci0 = 0; ci0 < Cin; ci0 += kCinTile) {
    for (int load4 = tid; load4 < kInArea * (kCinTile / 4); load4 += kThreads) {
      const int pixel = load4 / (kCinTile / 4);
      const int c4    = load4 - pixel * (kCinTile / 4);
      const int iy    = pixel / kInW;
      const int ix    = pixel - iy * kInW;
      const int gy    = oy0 + iy;
      const int gx    = ox0 + ix;
      float4    v     = make_float4(0.0F, 0.0F, 0.0F, 0.0F);
      if (gy < H && gx < W) {
        v = reinterpret_cast<const float4*>(
            input +
            (static_cast<std::int64_t>(n) * H * W + static_cast<std::int64_t>(gy) * W + gx) * Cin +
            ci0 + c4 * 4)[0];
      }
      reinterpret_cast<float4*>(in_s + pixel * kCinTile + c4 * 4)[0] = v;
    }
    for (int load4 = tid; load4 < kCinTile * 9 * kCout4; load4 += kThreads) {
      const int c4  = load4 % kCout4;
      const int tmp = load4 / kCout4;
      const int k   = tmp % 9;
      const int ci  = tmp / 9;
      reinterpret_cast<float4*>(w_s + (ci * 9 + k) * kCout + c4 * 4)[0] =
          reinterpret_cast<const float4*>(weight_ckco + ((ci0 + ci) * 9 + k) * kCout + c4 * 4)[0];
    }
    __syncthreads();

    if (pixel_valid) {
#pragma unroll
      for (int ci = 0; ci < kCinTile; ++ci) {
#pragma unroll
        for (int k = 0; k < 9; ++k) {
          const int   ky = k / 3;
          const int   kx = k - ky * 3;
          const float v  = in_s[((ty + ky) * kInW + tx + kx) * kCinTile + ci];
#pragma unroll
          for (int co4 = 0; co4 < kCout4; ++co4) {
            const float4 w =
                reinterpret_cast<const float4*>(w_s + (ci * 9 + k) * kCout + co4 * 4)[0];
            acc[co4 * 4]     = fmaf(v, w.x, acc[co4 * 4]);
            acc[co4 * 4 + 1] = fmaf(v, w.y, acc[co4 * 4 + 1]);
            acc[co4 * 4 + 2] = fmaf(v, w.z, acc[co4 * 4 + 2]);
            acc[co4 * 4 + 3] = fmaf(v, w.w, acc[co4 * 4 + 3]);
          }
        }
      }
    }
    __syncthreads();
  }

  if (pixel_valid) {
#pragma unroll
    for (int co4 = 0; co4 < kCout4; ++co4) {
      float4 v;
      v.x = fmaxf(acc[co4 * 4] + bias[co4 * 4], 0.0F);
      v.y = fmaxf(acc[co4 * 4 + 1] + bias[co4 * 4 + 1], 0.0F);
      v.z = fmaxf(acc[co4 * 4 + 2] + bias[co4 * 4 + 2], 0.0F);
      v.w = fmaxf(acc[co4 * 4 + 3] + bias[co4 * 4 + 3], 0.0F);
      reinterpret_cast<float4*>(
          output +
          (static_cast<std::int64_t>(n) * Ho * Wo + static_cast<std::int64_t>(oy) * Wo + ox) *
              kCout +
          co4 * 4)[0] = v;
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

// Launch the multi-Cout 3×3 tiled direct kernel.
// Tile sizes for demosaicnet-class shapes (large spatial, pad=0 valid):
//   OH=8, OW=16 → 128 threads/block
//   COUT tile fills student widths when possible (24 / 32)
//   CIN=8        → modest SMEM, frequent reuse of input apron
template <int kOhTile, int kOwTile, int kCoutTile, int kCinTile>
void LaunchConv2d3x3Tiled(const float* in_ptr, const float* w_ptr, const float* b_ptr,
                          float* out_ptr, int N, int Cin, int Cout, int H, int W, int Ho, int Wo,
                          bool add_bias, bool do_relu, cudaStream_t stream) {
  constexpr int kInH = kOhTile + 2;
  constexpr int kInW = kOwTile + 2;
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

[[nodiscard]] auto DirectTiledSmemBytes(const int cout_tile) -> int {
  constexpr int kOh      = 8;
  constexpr int kOw      = 16;
  constexpr int kCinTile = 8;
  constexpr int kInH     = kOh + 2;
  constexpr int kInW     = kOw + 2;
  return static_cast<int>((kCinTile * kInH * kInW + cout_tile * kCinTile * 9) * sizeof(float));
}

[[nodiscard]] auto NhwcTiledSmemBytes(const int channels, const int output_tile_width = 16) -> int {
  constexpr int kInHeight = 8 + 2;
  const int     kInArea   = kInHeight * (output_tile_width + 2);
  constexpr int kCinTile  = 8;
  return static_cast<int>((kInArea * kCinTile + kCinTile * 9 * channels) * sizeof(float));
}

void LaunchConv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                  bool do_relu, cudaStream_t stream, WorkspacePool* workspace) {
  // Workspace is accepted for API stability. Current kernels need no scratch and
  // never call cudaMalloc — satisfying the "no malloc when workspace provided" exit.
  (void)workspace;

  ValidateParams(input, output, params, do_relu ? "Conv2dBiasRelu" : "Conv2d");

  const int    N        = static_cast<int>(input.shape[0]);
  const int    Cin      = static_cast<int>(input.shape[1]);
  const int    H        = static_cast<int>(input.shape[2]);
  const int    W        = static_cast<int>(input.shape[3]);
  const int    Cout     = params.out_channels;
  const int    Ho       = static_cast<int>(output.shape[2]);
  const int    Wo       = static_cast<int>(output.shape[3]);

  const bool   add_bias = params.bias != nullptr;
  const float* in_ptr   = input.data;
  const float* w_ptr    = params.weight;
  const float* b_ptr    = params.bias;
  float*       out_ptr  = output.data;

  const bool   is_1x1   = (params.kH == 1 && params.kW == 1 && params.sH == 1 && params.sW == 1 &&
                       params.padH == 0 && params.padW == 0 && params.dilation == 1);
  const bool   is_2x2s2 = (params.kH == 2 && params.kW == 2 && params.sH == 2 && params.sW == 2 &&
                         params.padH == 0 && params.padW == 0 && params.dilation == 1);
  const bool   is_3x3s1 = (params.kH == 3 && params.kW == 3 && params.sH == 1 && params.sW == 1 &&
                         params.padH == 0 && params.padW == 0 && params.dilation == 1);

  if (is_1x1) {
    const int  spatial               = H * W;
    // Student residual/output layers have exact small Cout. The generic
    // spatial×Cout kernel uses 8-channel tiles, which wastes 5/8 lanes for
    // Cout=3 and reloads the input for the second partial tile at Cout=12.
    const bool is_student_small_cout = (Cin == 24 || Cin == 32) && (Cout == 3 || Cout == 12);
    if (is_student_small_cout) {
      const std::int64_t numel = static_cast<std::int64_t>(N) * spatial;
      if (numel > 0) {
        const int grid = ChooseGridSize(numel, kBlockSize);
        if (Cout == 3) {
          Conv2d1x1SmallCoutKernel<3><<<grid, kBlockSize, 0, stream>>>(
              in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, spatial, add_bias, do_relu);
        } else {
          Conv2d1x1SmallCoutKernel<12><<<grid, kBlockSize, 0, stream>>>(
              in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, spatial, add_bias, do_relu);
        }
        CheckCuda(cudaGetLastError(), "Conv2d1x1SmallCoutKernel launch");
      }
      return;
    }
    // Tile: 32 spatial × 8 cout → 256 threads/block
    constexpr int kTileSpatial = 32;
    constexpr int kTileCout    = 8;
    constexpr int kTileCin     = 16;
    dim3          block(kTileSpatial * kTileCout);
    dim3 grid((spatial + kTileSpatial - 1) / kTileSpatial, (Cout + kTileCout - 1) / kTileCout, N);
    if (grid.x > 0 && grid.y > 0 && grid.z > 0) {
      Conv2d1x1TiledKernel<kTileSpatial, kTileCout, kTileCin><<<grid, block, 0, stream>>>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, add_bias, do_relu);
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
    // Multi-Cout tiled direct conv. Dispatch by Cout tile so student shapes
    // fill blocks exactly: bayer_s24_d8 (Cout=24), xtrans_p2_s32_d4 (Cout=32).
    // Cin is always reduced in kCinTile strips — do not gate the Cout tile on
    // Cin (post_conv is 6→24/32).
    if (Cout >= 32) {
      LaunchConv2d3x3Tiled</*OH=*/8, /*OW=*/16, /*Cout=*/32, /*Cin=*/8>(
          in_ptr, w_ptr, b_ptr, out_ptr, N, Cin, Cout, H, W, Ho, Wo, add_bias, do_relu, stream);
    } else if (Cout >= 24) {
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

template <typename KernelT>
void FillKernelAttrs(KernelT kernel, const char* name, int cin, int cout, int threads_per_block,
                     int dynamic_smem_bytes, Conv2d3x3KernelInfo* out) {
  cudaFuncAttributes attr{};
  CheckCuda(cudaFuncGetAttributes(&attr, kernel), "cudaFuncGetAttributes");
  out->name                   = name;
  out->cin                    = cin;
  out->cout                   = cout;
  out->num_regs               = attr.numRegs;
  out->static_smem_bytes      = static_cast<int>(attr.sharedSizeBytes);
  out->max_dynamic_smem_bytes = attr.maxDynamicSharedSizeBytes;
  out->max_threads_per_block  = attr.maxThreadsPerBlock;
  out->threads_per_block      = threads_per_block;
  out->dynamic_smem_bytes     = dynamic_smem_bytes;
}

}  // namespace

void TransformConv2d3x3WeightsNhwc(const float* src_oihw, const int in_channels,
                                   const int out_channels, float* dst_ckco) {
  if (src_oihw == nullptr || dst_ckco == nullptr || in_channels != out_channels ||
      in_channels != 24) {
    throw std::runtime_error("TransformConv2d3x3WeightsNhwc: expected a C=24 square trunk");
  }
  for (int ci = 0; ci < in_channels; ++ci) {
    for (int k = 0; k < 9; ++k) {
      for (int co = 0; co < out_channels; ++co) {
        dst_ckco[(static_cast<std::size_t>(ci) * 9 + k) * out_channels + co] =
            src_oihw[(static_cast<std::size_t>(co) * in_channels + ci) * 9 + k];
      }
    }
  }
}

void Conv2d3x3NhwcBiasRelu(const float* input_nhwc, float* output_nhwc, const float* weight_ckco,
                           const float* bias, const int batch, const int height, const int width,
                           const int channels, const cudaStream_t stream) {
  if (input_nhwc == nullptr || output_nhwc == nullptr || weight_ckco == nullptr ||
      bias == nullptr || batch < 1 || height < 3 || width < 3 || channels != 24) {
    throw std::runtime_error("Conv2d3x3NhwcBiasRelu: expected valid C=24 NHWC tensors");
  }
  const int Ho = height - 2;
  const int Wo = width - 2;
  dim3      block(16, 8);
  dim3      grid((Wo + 15) / 16, (Ho + 7) / 8, static_cast<unsigned>(batch));
  Conv2d3x3s1NhwcTiledKernel<24, 8, 16><<<grid, block, NhwcTiledSmemBytes(24), stream>>>(
      input_nhwc, weight_ckco, bias, output_nhwc, batch, height, width, channels, Ho, Wo);
  CheckCuda(cudaGetLastError(), "Conv2d3x3s1NhwcTiledKernel launch");
}

void Conv2d(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
            cudaStream_t stream, WorkspacePool* workspace) {
  LaunchConv2d(input, output, params, /*do_relu=*/false, stream, workspace);
}

void Conv2dBiasRelu(const DeviceTensor& input, DeviceTensor& output, const Conv2dParams& params,
                    cudaStream_t stream, WorkspacePool* workspace) {
  LaunchConv2d(input, output, params, /*do_relu=*/true, stream, workspace);
}

auto QueryConv2d3x3KernelInfo(const int cin, const int cout, Conv2d3x3KernelInfo* out) -> bool {
  if (out == nullptr || cin <= 0 || cout <= 0) {
    return false;
  }
  *out = Conv2d3x3KernelInfo{};

  if (cout >= 32) {
    FillKernelAttrs(Conv2d3x3s1TiledKernel<8, 16, 32, 8>, "direct_tiled_32", cin, cout,
                    /*threads=*/8 * 16, DirectTiledSmemBytes(32), out);
  } else if (cout >= 24) {
    FillKernelAttrs(Conv2d3x3s1TiledKernel<8, 16, 24, 8>, "direct_tiled_24", cin, cout,
                    /*threads=*/8 * 16, DirectTiledSmemBytes(24), out);
  } else if (cout >= 16) {
    FillKernelAttrs(Conv2d3x3s1TiledKernel<8, 16, 16, 8>, "direct_tiled_16", cin, cout,
                    /*threads=*/8 * 16, DirectTiledSmemBytes(16), out);
  } else {
    FillKernelAttrs(Conv2d3x3s1TiledKernel<8, 16, 8, 8>, "direct_tiled_8", cin, cout,
                    /*threads=*/8 * 16, DirectTiledSmemBytes(8), out);
  }

  return true;
}

}  // namespace alcedo::cuda::nn
