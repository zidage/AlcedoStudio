//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
//  This file also contains material subject to the upstream notices below.

/**
 * RATIO CORRECTED DEMOSAICING
 * Luis Sanz Rodríguez (luis.sanz.rodriguez(at)gmail(dot)com)
 *
 * Release 2.3 @ 171125
 */

#include <cuda_runtime.h>

#include <array>
#include <cstdint>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <sstream>
#include <stdexcept>

#include "decoders/processor/operators/gpu/cuda_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/cuda_image_ops.hpp"
#include "decoders/processor/operators/gpu/cuda_raw_proc_utils.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace CUDA {

namespace {

constexpr float kEps              = 1e-5f;
constexpr float kEpsSq            = 1e-10f;
constexpr int   kRcdRadius        = 4;
constexpr int   kThreadsX         = 32;
constexpr int   kThreadsY         = 8;
constexpr int   kTileWidth        = kThreadsX + 2 * kRcdRadius;
constexpr int   kTileHeight       = kThreadsY + 2 * kRcdRadius;
constexpr int   kDirTileWidth     = kThreadsX + 2;
constexpr int   kDirTileHeight    = kThreadsY + 2;
constexpr int   kLpfTileWidth     = kThreadsX + 4;
constexpr int   kLpfTileHeight    = kThreadsY + 4;
// The per-stage timing waits on CUDA events; it runs only when its Debug log line can be
// written (Debug build, and alcedo.decode.debug enabled).
constexpr bool  kLogRcdKernelBreakdown =
    ::alcedo::log::kCompiledMinLevel == ::alcedo::log::Level::Debug;

__device__ __forceinline__ int FC(const BayerPattern2x2& pattern, const int y, const int x) {
  return pattern.rgb_fc[BayerCellIndex(y, x)];
}

__device__ __forceinline__ int ClampCoord(const int value, const int limit) {
  return max(0, min(value, limit - 1));
}

__device__ __forceinline__ void LoadRawTileFull(const cv::cuda::PtrStep<float> raw, const int width,
                                                const int height,
                                                float tile[kTileHeight][kTileWidth]) {
  const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - kRcdRadius;
  const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - kRcdRadius;

  for (int tile_y = threadIdx.y; tile_y < kTileHeight; tile_y += blockDim.y) {
    const int    gy       = ClampCoord(tile_y0 + tile_y, height);
    const float* raw_row  = raw.ptr(gy);
    float*       tile_row = tile[tile_y];
    for (int tile_x = threadIdx.x; tile_x < kTileWidth; tile_x += blockDim.x) {
      tile_row[tile_x] = raw_row[ClampCoord(tile_x0 + tile_x, width)];
    }
  }
}

__device__ __forceinline__ float LowPassAtTile(const float tile[kTileHeight][kTileWidth],
                                                const int y, const int x) {
  const float c  = tile[y][x];
  const float n  = tile[y - 1][x];
  const float s  = tile[y + 1][x];
  const float w  = tile[y][x - 1];
  const float e  = tile[y][x + 1];

  const float nw = tile[y - 1][x - 1];
  const float ne = tile[y - 1][x + 1];
  const float sw = tile[y + 1][x - 1];
  const float se = tile[y + 1][x + 1];

  return 0.25f * c + 0.125f * (n + s + w + e) + 0.0625f * (nw + ne + sw + se);
}

__device__ __forceinline__ float ClampRcdOutput(const float value, const bool clamp_high) {
  const float low_clamped = fmaxf(value, 0.0f);
  return clamp_high ? fminf(low_clamped, 1.0f) : low_clamped;
}

__device__ __forceinline__ float RcdDirectionalStat(const float c, const float m1, const float p1,
                                                    const float m2, const float p2,
                                                    const float m3, const float p3,
                                                    const float m4, const float p4) {
  float stat = 0.0f;
  stat += -18.f * c * m1;
  stat += -18.f * c * p1;
  stat += -36.f * c * m2;
  stat += -36.f * c * p2;
  stat += 18.f * c * m3;
  stat += 18.f * c * p3;
  stat += -2.f * c * m4;
  stat += -2.f * c * p4;
  stat += 38.f * c * c;
  stat += -70.f * m1 * p1;
  stat += -12.f * m1 * m2;
  stat += 24.f * m1 * p2;
  stat += -38.f * m1 * m3;
  stat += 16.f * m1 * p3;
  stat += 12.f * m1 * m4;
  stat += -6.f * m1 * p4;
  stat += 46.f * m1 * m1;
  stat += 24.f * p1 * m2;
  stat += -12.f * p1 * p2;
  stat += 16.f * p1 * m3;
  stat += -38.f * p1 * p3;
  stat += -6.f * p1 * m4;
  stat += 12.f * p1 * p4;
  stat += 46.f * p1 * p1;
  stat += 14.f * m2 * p2;
  stat += -12.f * m2 * p3;
  stat += -2.f * m2 * m4;
  stat += 2.f * m2 * p4;
  stat += 11.f * m2 * m2;
  stat += -12.f * p2 * m3;
  stat += 2.f * p2 * m4;
  stat += -2.f * p2 * p4;
  stat += 11.f * p2 * p2;
  stat += 2.f * m3 * p3;
  stat += -6.f * m3 * m4;
  stat += 10.f * m3 * m3;
  stat += -6.f * p3 * p4;
  stat += 10.f * p3 * p3;
  stat += m4 * m4;
  stat += p4 * p4;
  return fmaxf(stat, kEpsSq);
}

__global__ void RCD_InitAndVHKernel(const cv::cuda::PtrStep<float> raw, cv::cuda::PtrStep<float> r,
                                    cv::cuda::PtrStep<float> g, cv::cuda::PtrStep<float> b,
                                    cv::cuda::PtrStep<float> vh_dir,
                                    cv::cuda::PtrStep<float> pq_dir,
                                    const int width, const int height, BayerPattern2x2 pattern) {
  __shared__ float raw_tile[kTileHeight][kTileWidth];
  LoadRawTileFull(raw, width, height, raw_tile);
  __syncthreads();

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const int   tx    = threadIdx.x + kRcdRadius;
  const int   ty    = threadIdx.y + kRcdRadius;
  const float val   = raw_tile[ty][tx];
  const int   color = FC(pattern, y, x);

  r.ptr(y)[x]       = (color == 0) ? val : 0.0f;
  g.ptr(y)[x]       = (color == 1) ? val : 0.0f;
  b.ptr(y)[x]       = (color == 2) ? val : 0.0f;

  float vh          = 0.0f;
  float pq          = 0.0f;
  if (y >= kRcdRadius && y < height - kRcdRadius && x >= kRcdRadius && x < width - kRcdRadius) {
    {
      const float c   = val;

      const float vm1 = raw_tile[ty - 1][tx];
      const float vp1 = raw_tile[ty + 1][tx];
      const float vm2 = raw_tile[ty - 2][tx];
      const float vp2 = raw_tile[ty + 2][tx];
      const float vm3 = raw_tile[ty - 3][tx];
      const float vp3 = raw_tile[ty + 3][tx];
      const float vm4 = raw_tile[ty - 4][tx];
      const float vp4 = raw_tile[ty + 4][tx];

      const float hm1 = raw_tile[ty][tx - 1];
      const float hp1 = raw_tile[ty][tx + 1];
      const float hm2 = raw_tile[ty][tx - 2];
      const float hp2 = raw_tile[ty][tx + 2];
      const float hm3 = raw_tile[ty][tx - 3];
      const float hp3 = raw_tile[ty][tx + 3];
      const float hm4 = raw_tile[ty][tx - 4];
      const float hp4 = raw_tile[ty][tx + 4];

      const float V_stat = RcdDirectionalStat(c, vm1, vp1, vm2, vp2, vm3, vp3, vm4, vp4);
      const float H_stat = RcdDirectionalStat(c, hm1, hp1, hm2, hp2, hm3, hp3, hm4, hp4);

      vh = V_stat / (V_stat + H_stat);
    }

    if (color != 1) {
      const float c   = val;

      const float nw1 = raw_tile[ty - 1][tx - 1];
      const float se1 = raw_tile[ty + 1][tx + 1];
      const float nw2 = raw_tile[ty - 2][tx - 2];
      const float se2 = raw_tile[ty + 2][tx + 2];
      const float nw3 = raw_tile[ty - 3][tx - 3];
      const float se3 = raw_tile[ty + 3][tx + 3];
      const float nw4 = raw_tile[ty - 4][tx - 4];
      const float se4 = raw_tile[ty + 4][tx + 4];

      const float sw1 = raw_tile[ty + 1][tx - 1];
      const float ne1 = raw_tile[ty - 1][tx + 1];
      const float sw2 = raw_tile[ty + 2][tx - 2];
      const float ne2 = raw_tile[ty - 2][tx + 2];
      const float sw3 = raw_tile[ty + 3][tx - 3];
      const float ne3 = raw_tile[ty - 3][tx + 3];
      const float sw4 = raw_tile[ty + 4][tx - 4];
      const float ne4 = raw_tile[ty - 4][tx + 4];

      const float P_stat = RcdDirectionalStat(c, nw1, se1, nw2, se2, nw3, se3, nw4, se4);
      const float Q_stat = RcdDirectionalStat(c, sw1, ne1, sw2, ne2, sw3, ne3, sw4, ne4);

      pq = P_stat / (P_stat + Q_stat);
    }
  }

  vh_dir.ptr(y)[x] = vh;
  pq_dir.ptr(y)[x] = pq;
}

__global__ void RCD_GreenAtRBKernel(const cv::cuda::PtrStep<float> raw,
                                    const cv::cuda::PtrStep<float> vh_dir,
                                    cv::cuda::PtrStep<float> g, const int width, const int height,
                                    BayerPattern2x2 pattern, const bool clamp_high) {
  __shared__ float raw_tile[kTileHeight][kTileWidth];
  __shared__ float vh_tile[kDirTileHeight][kDirTileWidth];
  __shared__ float lpf_tile[kLpfTileHeight][kLpfTileWidth];
  LoadRawTileFull(raw, width, height, raw_tile);
  __syncthreads();

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 1;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 1;
    for (int tile_y = threadIdx.y; tile_y < kDirTileHeight; tile_y += blockDim.y) {
      const int    gy       = ClampCoord(tile_y0 + tile_y, height);
      const float* vh_row = vh_dir.ptr(gy);
      float*       tile_row = vh_tile[tile_y];
      for (int tile_x = threadIdx.x; tile_x < kDirTileWidth; tile_x += blockDim.x) {
        tile_row[tile_x] = vh_row[ClampCoord(tile_x0 + tile_x, width)];
      }
    }
  }

  for (int tile_y = threadIdx.y; tile_y < kLpfTileHeight; tile_y += blockDim.y) {
    float* tile_row = lpf_tile[tile_y];
    for (int tile_x = threadIdx.x; tile_x < kLpfTileWidth; tile_x += blockDim.x) {
      tile_row[tile_x] = LowPassAtTile(raw_tile, tile_y + 2, tile_x + 2);
    }
  }
  __syncthreads();

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  if (y < kRcdRadius || y >= height - kRcdRadius || x < kRcdRadius || x >= width - kRcdRadius) return;

  const int color = FC(pattern, y, x);
  if (color == 1) return;

  const int   dtx        = threadIdx.x + 1;
  const int   dty        = threadIdx.y + 1;
  const float VH_central = vh_tile[dty][dtx];
  const float VH_neigh   = 0.25f * (vh_tile[dty - 1][dtx - 1] + vh_tile[dty - 1][dtx + 1] +
                                  vh_tile[dty + 1][dtx - 1] + vh_tile[dty + 1][dtx + 1]);
  const float VH_disc =
      (fabsf(0.5f - VH_central) < fabsf(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const int tx  = threadIdx.x + kRcdRadius;
  const int ty  = threadIdx.y + kRcdRadius;
  const int ltx = threadIdx.x + 2;
  const int lty = threadIdx.y + 2;

  const float c      = raw_tile[ty][tx];

  const float vm1    = raw_tile[ty - 1][tx];
  const float vp1    = raw_tile[ty + 1][tx];
  const float vm2    = raw_tile[ty - 2][tx];
  const float vp2    = raw_tile[ty + 2][tx];
  const float vm3    = raw_tile[ty - 3][tx];
  const float vp3    = raw_tile[ty + 3][tx];
  const float vm4    = raw_tile[ty - 4][tx];
  const float vp4    = raw_tile[ty + 4][tx];

  const float hm1    = raw_tile[ty][tx - 1];
  const float hp1    = raw_tile[ty][tx + 1];
  const float hm2    = raw_tile[ty][tx - 2];
  const float hp2    = raw_tile[ty][tx + 2];
  const float hm3    = raw_tile[ty][tx - 3];
  const float hp3    = raw_tile[ty][tx + 3];
  const float hm4    = raw_tile[ty][tx - 4];
  const float hp4    = raw_tile[ty][tx + 4];

  const float lpf_c  = lpf_tile[lty][ltx];
  const float lpf_n2 = lpf_tile[lty - 2][ltx];
  const float lpf_s2 = lpf_tile[lty + 2][ltx];
  const float lpf_w2 = lpf_tile[lty][ltx - 2];
  const float lpf_e2 = lpf_tile[lty][ltx + 2];

  const float N_grad =
      kEps + fabsf(vm1 - vp1) + fabsf(c - vm2) + fabsf(vm1 - vm3) + fabsf(vm2 - vm4);
  const float S_grad =
      kEps + fabsf(vp1 - vm1) + fabsf(c - vp2) + fabsf(vp1 - vp3) + fabsf(vp2 - vp4);
  const float W_grad =
      kEps + fabsf(hm1 - hp1) + fabsf(c - hm2) + fabsf(hm1 - hm3) + fabsf(hm2 - hm4);
  const float E_grad =
      kEps + fabsf(hp1 - hm1) + fabsf(c - hp2) + fabsf(hp1 - hp3) + fabsf(hp2 - hp4);

  const float N_est  = vm1 * (1.f + (lpf_c - lpf_n2) / (kEps + lpf_c + lpf_n2));
  const float S_est  = vp1 * (1.f + (lpf_c - lpf_s2) / (kEps + lpf_c + lpf_s2));
  const float W_est  = hm1 * (1.f + (lpf_c - lpf_w2) / (kEps + lpf_c + lpf_w2));
  const float E_est  = hp1 * (1.f + (lpf_c - lpf_e2) / (kEps + lpf_c + lpf_e2));

  const float V_est  = (S_grad * N_est + N_grad * S_est) / (N_grad + S_grad);
  const float H_est  = (W_grad * E_est + E_grad * W_est) / (E_grad + W_grad);

  g.ptr(y)[x]        = ClampRcdOutput(VH_disc * H_est + (1.f - VH_disc) * V_est, clamp_high);
}

__global__ void RCD_RBAtRBKernel(const cv::cuda::PtrStep<float> pq_dir,
                                 const cv::cuda::PtrStep<float> g,
                                 cv::cuda::PtrStep<float> r, cv::cuda::PtrStep<float> b,
                                 const int width, const int height, BayerPattern2x2 pattern,
                                 const bool clamp_high) {
  __shared__ float pq_tile[kThreadsY + 2][kThreadsX + 2];
  __shared__ float g_tile[kThreadsY + 4][kThreadsX + 4];
  __shared__ float r_tile[kThreadsY + 7][kThreadsX + 7];
  __shared__ float b_tile[kThreadsY + 7][kThreadsX + 7];

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 1;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 1;
    for (int ty = threadIdx.y; ty < (kThreadsY + 2); ty += blockDim.y) {
      const int    gy      = ClampCoord(tile_y0 + ty, height);
      const float* pq_row = pq_dir.ptr(gy);
      float*       tile_row = pq_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 2); tx += blockDim.x) {
        tile_row[tx] = pq_row[ClampCoord(tile_x0 + tx, width)];
      }
    }
  }

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 2;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 2;
    for (int ty = threadIdx.y; ty < (kThreadsY + 4); ty += blockDim.y) {
      const int    gy       = ClampCoord(tile_y0 + ty, height);
      const float* g_row    = g.ptr(gy);
      float*       tile_row = g_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 4); tx += blockDim.x) {
        tile_row[tx] = g_row[ClampCoord(tile_x0 + tx, width)];
      }
    }
  }

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 3;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 3;
    for (int ty = threadIdx.y; ty < (kThreadsY + 7); ty += blockDim.y) {
      const int    gy     = ClampCoord(tile_y0 + ty, height);
      const float* r_row  = r.ptr(gy);
      const float* b_row  = b.ptr(gy);
      float*       r_trow = r_tile[ty];
      float*       b_trow = b_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 7); tx += blockDim.x) {
        const int gx = ClampCoord(tile_x0 + tx, width);
        r_trow[tx]   = r_row[gx];
        b_trow[tx]   = b_row[gx];
      }
    }
  }
  __syncthreads();

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  if (y < kRcdRadius || y >= height - kRcdRadius || x < kRcdRadius || x >= width - kRcdRadius)
    return;

  const int color = FC(pattern, y, x);
  if (color == 1) return;

  const int   c        = 2 - color;  // missing channel at RB position (R->B, B->R)
  const float (*ch_tile)[kThreadsX + 7] = (c == 0) ? r_tile : b_tile;

  const int ptx = threadIdx.x + 1;
  const int pty = threadIdx.y + 1;
  const int   gtx                       = threadIdx.x + 2;
  const int   gty                       = threadIdx.y + 2;
  const int   ctx                       = threadIdx.x + 3;
  const int   cty                       = threadIdx.y + 3;

  const float PQ_c                      = pq_tile[pty][ptx];
  const float PQ_n     = 0.25f * (pq_tile[pty - 1][ptx - 1] + pq_tile[pty - 1][ptx + 1] +
                              pq_tile[pty + 1][ptx - 1] + pq_tile[pty + 1][ptx + 1]);
  const float PQ_disc  = (fabsf(0.5f - PQ_c) < fabsf(0.5f - PQ_n)) ? PQ_n : PQ_c;

  const float g_c      = g_tile[gty][gtx];

  const float ch_nw1   = ch_tile[cty - 1][ctx - 1];
  const float ch_ne1   = ch_tile[cty - 1][ctx + 1];
  const float ch_sw1   = ch_tile[cty + 1][ctx - 1];
  const float ch_se1   = ch_tile[cty + 1][ctx + 1];

  const float ch_nw3   = ch_tile[cty - 3][ctx - 3];
  const float ch_ne3   = ch_tile[cty - 3][ctx + 3];
  const float ch_sw3   = ch_tile[cty + 3][ctx - 3];
  const float ch_se3   = ch_tile[cty + 3][ctx + 3];

  const float g_nw2    = g_tile[gty - 2][gtx - 2];
  const float g_ne2    = g_tile[gty - 2][gtx + 2];
  const float g_sw2    = g_tile[gty + 2][gtx - 2];
  const float g_se2    = g_tile[gty + 2][gtx + 2];

  const float NW_grad = kEps + fabsf(ch_nw1 - ch_se1) + fabsf(ch_nw1 - ch_nw3) + fabsf(g_c - g_nw2);
  const float NE_grad = kEps + fabsf(ch_ne1 - ch_sw1) + fabsf(ch_ne1 - ch_ne3) + fabsf(g_c - g_ne2);
  const float SW_grad = kEps + fabsf(ch_sw1 - ch_ne1) + fabsf(ch_sw1 - ch_sw3) + fabsf(g_c - g_sw2);
  const float SE_grad = kEps + fabsf(ch_se1 - ch_nw1) + fabsf(ch_se1 - ch_se3) + fabsf(g_c - g_se2);

  const float g_nw1   = g_tile[gty - 1][gtx - 1];
  const float g_ne1   = g_tile[gty - 1][gtx + 1];
  const float g_sw1   = g_tile[gty + 1][gtx - 1];
  const float g_se1   = g_tile[gty + 1][gtx + 1];

  const float NW_est  = ch_nw1 - g_nw1;
  const float NE_est  = ch_ne1 - g_ne1;
  const float SW_est  = ch_sw1 - g_sw1;
  const float SE_est  = ch_se1 - g_se1;

  const float P_est   = (NW_grad * SE_est + SE_grad * NW_est) / (NW_grad + SE_grad);
  const float Q_est   = (NE_grad * SW_est + SW_grad * NE_est) / (NE_grad + SW_grad);

  const float out_val =
      ClampRcdOutput(g_c + (1.f - PQ_disc) * P_est + PQ_disc * Q_est, clamp_high);

  if (c == 0) {
    r.ptr(y)[x] = out_val;
  } else {
    b.ptr(y)[x] = out_val;
  }
}

__global__ void RCD_RBAtGKernel(const cv::cuda::PtrStep<float> vh_dir,
                                const cv::cuda::PtrStep<float> g,
                                cv::cuda::PtrStep<float> r, cv::cuda::PtrStep<float> b,
                                const int width, const int height, BayerPattern2x2 pattern,
                                const bool clamp_high) {
  __shared__ float vh_tile[kThreadsY + 2][kThreadsX + 2];
  __shared__ float g_tile[kThreadsY + 4][kThreadsX + 4];
  __shared__ float r_tile[kThreadsY + 7][kThreadsX + 7];
  __shared__ float b_tile[kThreadsY + 7][kThreadsX + 7];

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 1;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 1;
    for (int ty = threadIdx.y; ty < (kThreadsY + 2); ty += blockDim.y) {
      const int    gy      = ClampCoord(tile_y0 + ty, height);
      const float* vh_row = vh_dir.ptr(gy);
      float*       tile_row = vh_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 2); tx += blockDim.x) {
        tile_row[tx] = vh_row[ClampCoord(tile_x0 + tx, width)];
      }
    }
  }

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 2;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 2;
    for (int ty = threadIdx.y; ty < (kThreadsY + 4); ty += blockDim.y) {
      const int    gy       = ClampCoord(tile_y0 + ty, height);
      const float* g_row    = g.ptr(gy);
      float*       tile_row = g_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 4); tx += blockDim.x) {
        tile_row[tx] = g_row[ClampCoord(tile_x0 + tx, width)];
      }
    }
  }

  {
    const int tile_x0 = static_cast<int>(blockIdx.x) * blockDim.x - 3;
    const int tile_y0 = static_cast<int>(blockIdx.y) * blockDim.y - 3;
    for (int ty = threadIdx.y; ty < (kThreadsY + 7); ty += blockDim.y) {
      const int    gy     = ClampCoord(tile_y0 + ty, height);
      const float* r_row  = r.ptr(gy);
      const float* b_row  = b.ptr(gy);
      float*       r_trow = r_tile[ty];
      float*       b_trow = b_tile[ty];
      for (int tx = threadIdx.x; tx < (kThreadsX + 7); tx += blockDim.x) {
        const int gx = ClampCoord(tile_x0 + tx, width);
        r_trow[tx]   = r_row[gx];
        b_trow[tx]   = b_row[gx];
      }
    }
  }
  __syncthreads();

  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  if (y < kRcdRadius || y >= height - kRcdRadius || x < kRcdRadius || x >= width - kRcdRadius)
    return;

  const int color = FC(pattern, y, x);
  if (color != 1) return;

  const int   vtx        = threadIdx.x + 1;
  const int   vty        = threadIdx.y + 1;
  const int   gtx        = threadIdx.x + 2;
  const int   gty        = threadIdx.y + 2;
  const int   ctx        = threadIdx.x + 3;
  const int   cty        = threadIdx.y + 3;

  const float VH_central = vh_tile[vty][vtx];
  const float VH_neigh   = 0.25f * (vh_tile[vty - 1][vtx - 1] + vh_tile[vty - 1][vtx + 1] +
                                  vh_tile[vty + 1][vtx - 1] + vh_tile[vty + 1][vtx + 1]);
  const float VH_disc = (fabsf(0.5f - VH_central) < fabsf(0.5f - VH_neigh)) ? VH_neigh : VH_central;

  const float g_c     = g_tile[gty][gtx];

  const float g_m2    = g_tile[gty - 2][gtx];
  const float g_p2    = g_tile[gty + 2][gtx];
  const float g_l2    = g_tile[gty][gtx - 2];
  const float g_r2    = g_tile[gty][gtx + 2];

  // --- R @ G ---
  {
    const float ch_m1  = r_tile[cty - 1][ctx];
    const float ch_p1  = r_tile[cty + 1][ctx];
    const float ch_m3  = r_tile[cty - 3][ctx];
    const float ch_p3  = r_tile[cty + 3][ctx];
    const float ch_l1  = r_tile[cty][ctx - 1];
    const float ch_r1  = r_tile[cty][ctx + 1];
    const float ch_l3  = r_tile[cty][ctx - 3];
    const float ch_r3  = r_tile[cty][ctx + 3];

    const float N_grad = kEps + fabsf(g_c - g_m2) + fabsf(ch_m1 - ch_p1) + fabsf(ch_m1 - ch_m3);
    const float S_grad = kEps + fabsf(g_c - g_p2) + fabsf(ch_p1 - ch_m1) + fabsf(ch_p1 - ch_p3);
    const float W_grad = kEps + fabsf(g_c - g_l2) + fabsf(ch_l1 - ch_r1) + fabsf(ch_l1 - ch_l3);
    const float E_grad = kEps + fabsf(g_c - g_r2) + fabsf(ch_r1 - ch_l1) + fabsf(ch_r1 - ch_r3);

    const float N_est  = ch_m1 - g_tile[gty - 1][gtx];
    const float S_est  = ch_p1 - g_tile[gty + 1][gtx];
    const float W_est  = ch_l1 - g_tile[gty][gtx - 1];
    const float E_est  = ch_r1 - g_tile[gty][gtx + 1];

    const float V_est  = (N_grad * S_est + S_grad * N_est) / (N_grad + S_grad);
    const float H_est  = (E_grad * W_est + W_grad * E_est) / (E_grad + W_grad);

    r.ptr(y)[x] = ClampRcdOutput(g_c + (1.f - VH_disc) * V_est + VH_disc * H_est, clamp_high);
  }

  // --- B @ G ---
  {
    const float ch_m1  = b_tile[cty - 1][ctx];
    const float ch_p1  = b_tile[cty + 1][ctx];
    const float ch_m3  = b_tile[cty - 3][ctx];
    const float ch_p3  = b_tile[cty + 3][ctx];
    const float ch_l1  = b_tile[cty][ctx - 1];
    const float ch_r1  = b_tile[cty][ctx + 1];
    const float ch_l3  = b_tile[cty][ctx - 3];
    const float ch_r3  = b_tile[cty][ctx + 3];

    const float N_grad = kEps + fabsf(g_c - g_m2) + fabsf(ch_m1 - ch_p1) + fabsf(ch_m1 - ch_m3);
    const float S_grad = kEps + fabsf(g_c - g_p2) + fabsf(ch_p1 - ch_m1) + fabsf(ch_p1 - ch_p3);
    const float W_grad = kEps + fabsf(g_c - g_l2) + fabsf(ch_l1 - ch_r1) + fabsf(ch_l1 - ch_l3);
    const float E_grad = kEps + fabsf(g_c - g_r2) + fabsf(ch_r1 - ch_l1) + fabsf(ch_r1 - ch_r3);

    const float N_est  = ch_m1 - g_tile[gty - 1][gtx];
    const float S_est  = ch_p1 - g_tile[gty + 1][gtx];
    const float W_est  = ch_l1 - g_tile[gty][gtx - 1];
    const float E_est  = ch_r1 - g_tile[gty][gtx + 1];

    const float V_est  = (N_grad * S_est + S_grad * N_est) / (N_grad + S_grad);
    const float H_est  = (E_grad * W_est + W_grad * E_est) / (E_grad + W_grad);

    b.ptr(y)[x] = ClampRcdOutput(g_c + (1.f - VH_disc) * V_est + VH_disc * H_est, clamp_high);
  }
}

}  // namespace

namespace {

void RunRcdKernels(const cv::cuda::GpuMat& raw, const BayerPattern2x2& pattern,
                   RcdWorkspace& workspace, cv::cuda::GpuMat* merged_output,
                   cv::cuda::Stream& stream) {
  const int width  = raw.cols;
  const int height = raw.rows;
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width <= 2 * kRcdRadius || height <= 2 * kRcdRadius) {
    throw std::runtime_error("CUDA RCD: image too small for RCD radius.");
  }

  workspace.Reserve(cv::Size(width, height));

  cv::cuda::GpuMat& r      = workspace.r;
  cv::cuda::GpuMat& g      = workspace.g;
  cv::cuda::GpuMat& b      = workspace.b;
  cv::cuda::GpuMat& vh_dir = workspace.vh_dir;
  cv::cuda::GpuMat& pq_dir = workspace.pq_dir;

  const cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);
  constexpr std::array<const char*, 5> kStageNames = {"RAW CUDA RCD init+dir",
                                                      "RAW CUDA RCD green@rb",
                                                      "RAW CUDA RCD rb@rb",
                                                      "RAW CUDA RCD rb@g",
                                                      "RAW CUDA RCD merge"};
  const size_t stage_count = merged_output == nullptr ? 4 : 5;
  std::array<cudaEvent_t, 6> stage_events{};
  const bool                           log_breakdown =
      kLogRcdKernelBreakdown &&
      ::alcedo::log::IsEnabled(::alcedo::log::Level::Debug, ::alcedo::log::Category::Decode);

  if (log_breakdown) {
    for (cudaEvent_t& event : stage_events) {
      CUDA_CHECK(cudaEventCreate(&event));
    }
    CUDA_CHECK(cudaEventRecord(stage_events[0], cuda_stream));
  }

  const dim3 threads(kThreadsX, kThreadsY);
  const dim3 blocks((width + threads.x - 1) / threads.x, (height + threads.y - 1) / threads.y);
  const bool clamp_high = merged_output != nullptr;

  RCD_InitAndVHKernel<<<blocks, threads, 0, cuda_stream>>>(raw, r, g, b, vh_dir, pq_dir, width,
                                                            height, pattern);
  CUDA_CHECK(cudaGetLastError());
  if (log_breakdown) {
    CUDA_CHECK(cudaEventRecord(stage_events[1], cuda_stream));
  }

  RCD_GreenAtRBKernel<<<blocks, threads, 0, cuda_stream>>>(raw, vh_dir, g, width, height, pattern,
                                                            clamp_high);
  CUDA_CHECK(cudaGetLastError());
  if (log_breakdown) {
    CUDA_CHECK(cudaEventRecord(stage_events[2], cuda_stream));
  }

  RCD_RBAtRBKernel<<<blocks, threads, 0, cuda_stream>>>(pq_dir, g, r, b, width, height, pattern,
                                                         clamp_high);
  CUDA_CHECK(cudaGetLastError());
  if (log_breakdown) {
    CUDA_CHECK(cudaEventRecord(stage_events[3], cuda_stream));
  }

  RCD_RBAtGKernel<<<blocks, threads, 0, cuda_stream>>>(vh_dir, g, r, b, width, height, pattern,
                                                        clamp_high);
  CUDA_CHECK(cudaGetLastError());
  if (log_breakdown) {
    CUDA_CHECK(cudaEventRecord(stage_events[4], cuda_stream));
  }

  const cv::Rect valid_roi(kRcdRadius, kRcdRadius, width - 2 * kRcdRadius,
                           height - 2 * kRcdRadius);
  cv::cuda::GpuMat r_valid = r(valid_roi);
  cv::cuda::GpuMat g_valid = g(valid_roi);
  cv::cuda::GpuMat b_valid = b(valid_roi);

  if (merged_output != nullptr) {
    MergeRGB(r_valid, g_valid, b_valid, *merged_output, &stream);
    if (log_breakdown) {
      CUDA_CHECK(cudaEventRecord(stage_events[5], cuda_stream));
    }
  } else {
    workspace.r = std::move(r_valid);
    workspace.g = std::move(g_valid);
    workspace.b = std::move(b_valid);
  }

  if (log_breakdown) {
    const size_t last_event = merged_output == nullptr ? 4 : 5;
    CUDA_CHECK(cudaEventSynchronize(stage_events[last_event]));

    std::ostringstream oss;
    float subtotal_ms = 0.0f;
    for (size_t i = 0; i < stage_count; ++i) {
      float elapsed_ms = 0.0f;
      CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, stage_events[i], stage_events[i + 1]));
      if (i != 0) {
        oss << " | ";
      }
      oss << kStageNames[i] << '=' << elapsed_ms << " ms";
      subtotal_ms += elapsed_ms;
    }
    oss << " | RAW CUDA RCD subtotal=" << subtotal_ms << " ms";
    ALCEDO_LOG_DEBUG(Decode) << oss.str();

    for (cudaEvent_t& event : stage_events) {
      cudaEventDestroy(event);
    }
  }
}

}  // namespace

void Bayer2x2ToRGB_RCD(cv::cuda::GpuMat& image, const BayerPattern2x2& pattern,
                       RcdWorkspace* workspace, cv::cuda::Stream* stream) {
  CV_Assert(image.type() == CV_32FC1);

  RcdWorkspace local_workspace;
  RcdWorkspace& active_workspace = workspace == nullptr ? local_workspace : *workspace;
  cv::cuda::Stream local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;

  RunRcdKernels(image, pattern, active_workspace, &image, active_stream);
  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
}

void Bayer2x2ToPlanarRGB_RCD(const cv::cuda::GpuMat& raw, const BayerPattern2x2& pattern,
                             RcdWorkspace* workspace, cv::cuda::Stream* stream) {
  CV_Assert(raw.type() == CV_32FC1);

  const int width  = raw.cols;
  const int height = raw.rows;
  if (width <= 0 || height <= 0) {
    return;
  }

  RcdWorkspace local_workspace;
  RcdWorkspace& active_workspace = workspace == nullptr ? local_workspace : *workspace;
  cv::cuda::Stream local_stream;
  cv::cuda::Stream& active_stream = stream == nullptr ? local_stream : *stream;

  RunRcdKernels(raw, pattern, active_workspace, nullptr, active_stream);
  if (stream == nullptr) {
    active_stream.waitForCompletion();
  }
}
}  // namespace CUDA
}  // namespace alcedo
