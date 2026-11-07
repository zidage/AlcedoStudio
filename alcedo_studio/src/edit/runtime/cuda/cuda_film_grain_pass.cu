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
constexpr int kTile             = 16;
constexpr int kThreads          = kTile * kTile;
/// The side of the noise tile at the largest halo.
constexpr int kMaxSpan          = kTile + 2 * FILM_GRAIN_MAX_BLUR_RADIUS;
/// Horizontal blur outputs of one thread at the largest halo: kMaxSpan rows of kTile columns.
constexpr int kMaxBlurPerThread = (kMaxSpan * kTile + kThreads - 1) / kThreads;

/// Shared memory of one block: three channels of the noise tile of side kTile + 2 halo. The
/// horizontal blur overwrites the noise in place, row major with kTile columns.
auto          SharedBytes(int halo) -> std::size_t {
  const auto span = static_cast<std::size_t>(kTile + 2 * halo);
  return 3U * span * span * sizeof(float);
}

/**
 * The post grain of one 16 x 16 tile, in one kernel: the noise of the tile and its halo (G-noise),
 * the horizontal blur (G-blur-x), the vertical blur with the division by the sum of w^2 and the
 * mix (G-blur-y), and the apply step. The noise reads the reference pixels of the render pixels,
 * so a tile at the edge of a viewport region reads the same noise as in the full frame. The
 * weights past the radius of a channel are 0.
 */
__global__ void FilmGrainPostKernel(const float4* src, float4* dst, int width, int height,
                                    FilmGrainFieldParams field, FilmGrainPostParams post) {
  extern __shared__ float shared[];
  const int               halo  = field.halo;
  const int               span  = kTile + 2 * halo;
  const int               area  = span * span;
  float*                  red   = shared;
  float*                  green = shared + area;
  float*                  blue  = shared + 2 * area;
  const int               local = static_cast<int>(threadIdx.y * kTile + threadIdx.x);
  const int               x0    = static_cast<int>(blockIdx.x) * kTile - halo;
  const int               y0    = static_cast<int>(blockIdx.y) * kTile - halo;

  // G-noise: one aggregate noise for each pixel of the tile and its halo.
  for (int index = local; index < area; index += kThreads) {
    const FilmGrainNoise3 noise =
        FilmGrainAggregateNoise(field.noise, x0 + index % span, y0 + index / span);
    red[index]   = noise.x;
    green[index] = noise.y;
    blue[index]  = noise.z;
  }
  __syncthreads();

  // G-blur-x: span rows of kTile columns. The results stay in registers until every thread has
  // read its taps, then overwrite the noise.
  const int outputs = span * kTile;
  float     blur_r[kMaxBlurPerThread];
  float     blur_g[kMaxBlurPerThread];
  float     blur_b[kMaxBlurPerThread];
  int       count = 0;
  for (int index = local; index < outputs; index += kThreads, ++count) {
    const int centre = (index / kTile) * span + index % kTile + halo;
    float     r      = red[centre] * field.weights[0][0];
    float     g      = green[centre] * field.weights[1][0];
    float     b      = blue[centre] * field.weights[2][0];
    for (int tap = 1; tap <= halo; ++tap) {
      r += (red[centre - tap] + red[centre + tap]) * field.weights[0][tap];
      g += (green[centre - tap] + green[centre + tap]) * field.weights[1][tap];
      b += (blue[centre - tap] + blue[centre + tap]) * field.weights[2][tap];
    }
    blur_r[count] = r;
    blur_g[count] = g;
    blur_b[count] = b;
  }
  __syncthreads();
  count = 0;
  for (int index = local; index < outputs; index += kThreads, ++count) {
    red[index]   = blur_r[count];
    green[index] = blur_g[count];
    blue[index]  = blur_b[count];
  }
  __syncthreads();

  const int x = static_cast<int>(blockIdx.x) * kTile + static_cast<int>(threadIdx.x);
  const int y = static_cast<int>(blockIdx.y) * kTile + static_cast<int>(threadIdx.y);
  if (x >= width || y >= height) return;

  // G-blur-y, the unit field, and the mix of rule 'grain_layer_mix'.
  const int centre = (static_cast<int>(threadIdx.y) + halo) * kTile + static_cast<int>(threadIdx.x);
  float     r      = red[centre] * field.weights[0][0];
  float     g      = green[centre] * field.weights[1][0];
  float     b      = blue[centre] * field.weights[2][0];
  for (int tap = 1; tap <= halo; ++tap) {
    const int above = centre - tap * kTile;
    const int below = centre + tap * kTile;
    r += (red[above] + red[below]) * field.weights[0][tap];
    g += (green[above] + green[below]) * field.weights[1][tap];
    b += (blue[above] + blue[below]) * field.weights[2][tap];
  }
  FilmGrainNoise3 unit;
  unit.x                       = r * field.inverse_weight_square_sum[0];
  unit.y                       = g * field.inverse_weight_square_sum[1];
  unit.z                       = b * field.inverse_weight_square_sum[2];
  const FilmGrainNoise3  grain = FilmGrainMix(field.mix, unit);

  const auto             index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
  const float4           pixel = src[index];
  const FilmGrainPostRgb result =
      FilmGrainPostPixel(post, FilmGrainPostRgb{pixel.x, pixel.y, pixel.z}, grain);
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
