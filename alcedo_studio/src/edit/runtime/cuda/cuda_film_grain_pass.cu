//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "cuda/cuda_check.hpp"
#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_post.h"

namespace alcedo {
namespace {

/// The side of the render tile of one block.
constexpr int kTile    = 16;
constexpr int kThreads = kTile * kTile;

/// Dynamic shared memory of one block: the noise band (kTile rows of kTile + 2 halo columns) and
/// the horizontal blur (kTile + 2 halo rows of kTile columns), three channels each. At the radius
/// 24 that is 24 KB; with the apply parameters it stays inside the 48 KB of every CUDA device.
auto          SharedBytes(int halo) -> std::size_t {
  const auto band = static_cast<std::size_t>(kTile) * static_cast<std::size_t>(kTile + 2 * halo);
  return 2U * 3U * band * sizeof(float);
}

/**
 * The post grain of one 16 x 16 tile, in one kernel: the noise of the tile and its halo (G-noise),
 * the horizontal blur (G-blur-x), the vertical blur with the division by the sum of w^2 and the
 * mix (G-blur-y), and the apply step. The noise reads the reference pixels of the render pixels,
 * so a tile at the edge of a viewport region reads the same noise as in the full frame. The block
 * makes the noise one band of kTile rows at a time and blurs each band across, the layout of
 * film_grain_post.cl and film_grain_post.metal. The weights past the radius of a channel are 0.
 */
__global__ void FilmGrainPostKernel(const float4* src, float4* dst, int width, int height,
                                    FilmGrainFieldParams field, FilmGrainPostParams post) {
  extern __shared__ float        shared[];
  // The apply step reads its parameters through a pointer. A pointer to a kernel parameter would
  // make a copy for each thread, so one thread copies them once into shared memory.
  __shared__ FilmGrainPostParams post_shared;
  const int                      halo   = field.halo;
  const int                      span   = kTile + 2 * halo;
  // One band of the noise (kTile rows of span columns) and the horizontal blur (span rows of kTile
  // columns) have the same number of texels.
  const int                      band   = kTile * span;
  float*                         noise  = shared;
  float*                         across = shared + 3 * band;
  const int                      local  = static_cast<int>(threadIdx.y * kTile + threadIdx.x);
  const int                      x0     = static_cast<int>(blockIdx.x) * kTile - halo;
  const int                      y0     = static_cast<int>(blockIdx.y) * kTile - halo;
  // A plain assignment reads the parameter space; the first __syncthreads publishes it.
  if (local == 0) post_shared = post;

  for (int band_row = 0; band_row < span; band_row += kTile) {
    const int rows = min(kTile, span - band_row);
    // G-noise: one aggregate noise for each texel of the band.
    for (int index = local; index < rows * span; index += kThreads) {
      const FilmGrainNoise3 value =
          FilmGrainAggregateNoise(field.noise, x0 + index % span, y0 + band_row + index / span);
      noise[index]            = value.x;
      noise[band + index]     = value.y;
      noise[2 * band + index] = value.z;
    }
    __syncthreads();
    // G-blur-x of the band at the kTile centre columns.
    for (int index = local; index < rows * kTile; index += kThreads) {
      const int row    = index / kTile;
      const int column = index % kTile;
      const int centre = row * span + column + halo;
      float     r      = noise[centre] * field.weights[0][0];
      float     g      = noise[band + centre] * field.weights[1][0];
      float     b      = noise[2 * band + centre] * field.weights[2][0];
      for (int tap = 1; tap <= halo; ++tap) {
        r += (noise[centre - tap] + noise[centre + tap]) * field.weights[0][tap];
        g += (noise[band + centre - tap] + noise[band + centre + tap]) * field.weights[1][tap];
        b += (noise[2 * band + centre - tap] + noise[2 * band + centre + tap]) *
             field.weights[2][tap];
      }
      const int out          = (band_row + row) * kTile + column;
      across[out]            = r;
      across[band + out]     = g;
      across[2 * band + out] = b;
    }
    // The next band overwrites the noise, and the vertical blur reads every band.
    __syncthreads();
  }

  const int x = static_cast<int>(blockIdx.x) * kTile + static_cast<int>(threadIdx.x);
  const int y = static_cast<int>(blockIdx.y) * kTile + static_cast<int>(threadIdx.y);
  if (x >= width || y >= height) return;

  // G-blur-y, the unit field, and the mix of rule 'grain_layer_mix'.
  const int centre = (static_cast<int>(threadIdx.y) + halo) * kTile + static_cast<int>(threadIdx.x);
  float     r      = across[centre] * field.weights[0][0];
  float     g      = across[band + centre] * field.weights[1][0];
  float     b      = across[2 * band + centre] * field.weights[2][0];
  for (int tap = 1; tap <= halo; ++tap) {
    const int above = centre - tap * kTile;
    const int below = centre + tap * kTile;
    r += (across[above] + across[below]) * field.weights[0][tap];
    g += (across[band + above] + across[band + below]) * field.weights[1][tap];
    b += (across[2 * band + above] + across[2 * band + below]) * field.weights[2][tap];
  }
  FilmGrainNoise3 unit;
  unit.x                       = r * field.inverse_weight_square_sum[0];
  unit.y                       = g * field.inverse_weight_square_sum[1];
  unit.z                       = b * field.inverse_weight_square_sum[2];
  const FilmGrainNoise3  grain = FilmGrainMix(field.mix, unit);

  const auto             index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
  const float4           pixel = src[index];
  const FilmGrainPostRgb result =
      FilmGrainPostPixel(&post_shared, FilmGrainPostRgb{pixel.x, pixel.y, pixel.z}, grain);
  dst[index] = make_float4(result.r, result.g, result.b, pixel.w);
}

}  // namespace

void RecordCudaFilmGrainPost(CudaRenderDevice& device, const FilmGrainPlan& plan,
                             const ResolvedRenderGeometry& geometry,
                             FilmGrainDisplayEncoding encoding, const CudaBackend::Texture2D& src,
                             CudaBackend::Texture2D& dst, std::uint32_t width,
                             std::uint32_t height) {
  if (!plan.enabled) {
    throw std::invalid_argument("RecordCudaFilmGrainPost: the grain plan is disabled");
  }
  if (width == 0 || height == 0 || src.Width() != width || src.Height() != height ||
      dst.Width() != width || dst.Height() != height) {
    throw std::invalid_argument("RecordCudaFilmGrainPost: the images do not have the render size");
  }
  const FilmGrainFieldParams field = MakeFilmGrainFieldParams(plan, geometry);
  const FilmGrainPostParams  post =
      MakeFilmGrainPostParams(plan, encoding.transfer, encoding.decode_scale);
  if (field.halo < 0 || field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument("RecordCudaFilmGrainPost: the blur radius is outside 0 to 24");
  }
  const dim3 block{kTile, kTile};
  const dim3 grid{(width + kTile - 1) / kTile, (height + kTile - 1) / kTile};
  FilmGrainPostKernel<<<grid, block, SharedBytes(field.halo), device.CommandContext().Stream()>>>(
      static_cast<const float4*>(src.DevicePointer()), static_cast<float4*>(dst.DevicePointer()),
      static_cast<int>(width), static_cast<int>(height), field, post);
  cuda::CheckCuda(::cudaGetLastError(), "RecordCudaFilmGrainPost: kernel launch");
}

}  // namespace alcedo
