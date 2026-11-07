//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// The tile kernel of the film grain on CUDA (plan 'film_grain_format_and_stock_plan.md', section
// 6.3): the noise, the two blurs, the unit field, and the mix of one tile, then an apply step of
// the caller. The DRT/Post Film Grain (cuda_film_grain_pass.cu) and the FP6 display kernels of the
// film print (cuda_film_print_pass.cu, phase G5) instantiate it with their own apply step, so
// every grain on CUDA reads one field. Include it only from .cu files.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "cuda/cuda_check.hpp"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo::cuda_film_grain {

/// The columns of the render tile of one block, and the threads of one block on each axis.
inline constexpr int            kTile    = 16;
inline constexpr int            kThreads = kTile * kTile;
/// Rows of one noise band for a tile of @p tile_rows rows: the whole tile with its halo up to 48
/// rows, else kTile. One band has one barrier pair and keeps every thread busy; at 6000 px the halo
/// is 2 to 4.
__host__ __device__ inline auto BandRows(int halo, int tile_rows) -> int {
  const int rows = tile_rows + 2 * halo;
  return rows <= 3 * kTile ? rows : kTile;
}

/// Dynamic shared memory of one block: the noise band (BandRows rows of kTile + 2 halo columns)
/// and the horizontal blur (@p tile_rows + 2 halo rows of kTile columns), three channels each. At
/// most 37 KB (a 16-row tile with the halo 16); with the apply parameters it stays inside the
/// 48 KB of every CUDA device.
inline auto SharedBytes(int halo, int tile_rows) -> std::size_t {
  const auto columns = static_cast<std::size_t>(kTile + 2 * halo);
  const auto rows    = static_cast<std::size_t>(tile_rows + 2 * halo);
  return 3U * (static_cast<std::size_t>(BandRows(halo, tile_rows)) * columns + rows * kTile) *
         sizeof(float);
}

/**
 * The grain of one tile of 16 columns and `Apply::kTileRows` rows, in one kernel: the noise of the
 * tile and its halo (G-noise), the
 * horizontal blur (G-blur-x), the vertical blur with the division by the sum of w^2 and the mix
 * (G-blur-y), and the apply step @p apply. The noise reads the reference pixels of the render
 * pixels, so a tile at the edge of a viewport region reads the same noise as in the full frame. The
 * block makes the noise one band of rows at a time and blurs each band across, the layout of
 * film_grain_post.cl and film_grain_post.metal; up to the halo 8 one band holds the whole tile
 * (@ref BandRows). The weights past the radius of a channel are 0. Each sum has the order of the
 * host form, so the result does not depend on the place of a pixel in its tile.
 *
 * @tparam Apply A value type with the member type `Params`, the constant `kTileRows` (16 or 32),
 *         and the device call `(const Params* params, int x, int y, FilmGrainNoise3 grain)` for
 *         each render pixel (x, y). @p params is in shared memory. A step that reads and writes
 *         only its own pixel can work in place. With 32 rows, each thread applies the grain to the
 *         pixels of rows y and y + 16, so the noise of the halo and the horizontal blur serve
 *         twice as many pixels: the FP6 kernels of the film print use it (0.5 ms less at 16mm and
 *         6000 px than 16 rows, phase G5). The post grain keeps 16 rows: with 32 rows its apply
 *         step took twice as long.
 */
template <class Apply>
__global__ void FilmGrainTileKernel(Apply apply, typename Apply::Params params, int width,
                                    int height, FilmGrainFieldParams field) {
  constexpr int kTileRows = Apply::kTileRows;
  static_assert(kTileRows == kTile || kTileRows == 2 * kTile);
  extern __shared__ float           shared[];
  // The apply step reads its parameters through a pointer. A pointer to a kernel parameter would
  // make a copy for each thread, so one thread copies them once into shared memory.
  __shared__ typename Apply::Params params_shared;
  const int                         halo      = field.halo;
  const int                         columns   = kTile + 2 * halo;
  const int                         span_rows = kTileRows + 2 * halo;
  // One band of the noise (band_rows rows of `columns` texels), and the horizontal blur (span_rows
  // rows of kTile texels). Each holds three planes, one per channel.
  const int                         band_rows = BandRows(halo, kTileRows);
  const int                         plane     = band_rows * columns;
  const int                         band      = kTile * span_rows;
  float*                            noise     = shared;
  float*                            across    = shared + 3 * plane;
  const int                         local     = static_cast<int>(threadIdx.y * kTile + threadIdx.x);
  const int                         x0        = static_cast<int>(blockIdx.x) * kTile - halo;
  const int                         y0        = static_cast<int>(blockIdx.y) * kTileRows - halo;
  // A plain assignment reads the parameter space; the first __syncthreads publishes it.
  if (local == 0) params_shared = params;

  for (int band_row = 0; band_row < span_rows; band_row += band_rows) {
    const int rows = min(band_rows, span_rows - band_row);
    // G-noise: one aggregate noise for each texel of the band.
    for (int index = local; index < rows * columns; index += kThreads) {
      const FilmGrainNoise3 value = FilmGrainAggregateNoise(field.noise, x0 + index % columns,
                                                            y0 + band_row + index / columns);
      noise[index]                = value.x;
      noise[plane + index]        = value.y;
      noise[2 * plane + index]    = value.z;
    }
    __syncthreads();
    // G-blur-x of the band at the kTile centre columns.
    for (int index = local; index < rows * kTile; index += kThreads) {
      const int row    = index / kTile;
      const int column = index % kTile;
      const int centre = row * columns + column + halo;
      float     r      = noise[centre] * field.weights[0][0];
      float     g      = noise[plane + centre] * field.weights[1][0];
      float     b      = noise[2 * plane + centre] * field.weights[2][0];
      for (int tap = 1; tap <= halo; ++tap) {
        r += (noise[centre - tap] + noise[centre + tap]) * field.weights[0][tap];
        g += (noise[plane + centre - tap] + noise[plane + centre + tap]) * field.weights[1][tap];
        b += (noise[2 * plane + centre - tap] + noise[2 * plane + centre + tap]) *
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
  if (x >= width) return;
#pragma unroll
  for (int half = 0; half < kTileRows / kTile; ++half) {
    const int tile_row = static_cast<int>(threadIdx.y) + half * kTile;
    const int y        = static_cast<int>(blockIdx.y) * kTileRows + tile_row;
    if (y >= height) return;
    // G-blur-y, the unit field, and the mix of rule 'grain_layer_mix'.
    const int centre = (tile_row + halo) * kTile + static_cast<int>(threadIdx.x);
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
    unit.x = r * field.inverse_weight_square_sum[0];
    unit.y = g * field.inverse_weight_square_sum[1];
    unit.z = b * field.inverse_weight_square_sum[2];
    apply(&params_shared, x, y, FilmGrainMix(field.mix, unit));
  }
}

/**
 * The field parameters of @p plan for a render of @p width x @p height, with the checks of every
 * grain kernel.
 * @throws std::invalid_argument that starts with @p caller for a disabled plan, an empty render,
 *         or a blur radius outside 0 to 24.
 */
inline auto FieldParamsFor(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                           std::uint32_t width, std::uint32_t height, const char* caller)
    -> FilmGrainFieldParams {
  if (!plan.enabled) {
    throw std::invalid_argument(std::string(caller) + ": the grain plan is disabled");
  }
  if (width == 0 || height == 0) {
    throw std::invalid_argument(std::string(caller) + ": the render is empty");
  }
  const FilmGrainFieldParams field = MakeFilmGrainFieldParams(plan, geometry);
  if (field.halo < 0 || field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument(std::string(caller) + ": the blur radius is outside 0 to 24");
  }
  return field;
}

/// Launch @ref FilmGrainTileKernel for a render of @p width x @p height on @p stream.
/// @throws std::runtime_error that starts with @p caller for a launch failure.
template <class Apply>
void Launch(cudaStream_t stream, const Apply& apply, const typename Apply::Params& params,
            const FilmGrainFieldParams& field, std::uint32_t width, std::uint32_t height,
            const char* caller) {
  constexpr unsigned kTileRows = Apply::kTileRows;
  const dim3         block{kTile, kTile};
  const dim3         grid{(width + kTile - 1) / kTile, (height + kTileRows - 1) / kTileRows};
  FilmGrainTileKernel<Apply><<<grid, block, SharedBytes(field.halo, kTileRows), stream>>>(
      apply, params, static_cast<int>(width), static_cast<int>(height), field);
  cuda::CheckCuda(::cudaGetLastError(), (std::string(caller) + ": kernel launch").c_str());
}

}  // namespace alcedo::cuda_film_grain
