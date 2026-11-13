//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_film_grain_tile.cuh"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_negative.h"
#include "edit/runtime/film/film_grain_post.h"

namespace alcedo {
namespace {

/// The apply step of the DRT/Post Film Grain: display code values, @ref src to @ref dst.
struct PostApply {
  using Params                              = FilmGrainPostParams;
  static constexpr int            kTileRows = cuda_film_grain::kTile;
  const float4*                   src;
  float4*                         dst;
  int                             width;

  __device__ __forceinline__ void operator()(const Params* params, int x, int y,
                                             FilmGrainNoise3 grain) const {
    const auto   index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
    const float4 pixel = src[index];
    const FilmGrainPostRgb result =
        FilmGrainPostPixel(params, FilmGrainPostRgb{pixel.x, pixel.y, pixel.z}, grain);
    dst[index] = make_float4(result.r, result.g, result.b, pixel.w);
  }
};

/// The apply step of the grain in the negative alone: the dye amounts, in place. The images between
/// the steps of the film print hold three packed floats per pixel.
struct NegativeApply {
  using Params                              = FilmGrainNegativeParams;
  static constexpr int            kTileRows = 2 * cuda_film_grain::kTile;
  float3*                         amounts;
  int                             width;

  __device__ __forceinline__ void operator()(const Params* params, int x, int y,
                                             FilmGrainNoise3 grain) const {
    const auto   index  = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
    const float3 record = amounts[index];
    const FilmGrainNegativeRgb result =
        FilmGrainNegativeRecord(params, FilmGrainNegativeRgb{record.x, record.y, record.z}, grain);
    amounts[index] = make_float3(result.r, result.g, result.b);
  }
};

}  // namespace

void RecordCudaFilmGrainPost(CudaRenderDevice& device, const FilmGrainPlan& plan,
                             const ResolvedRenderGeometry& geometry,
                             FilmGrainDisplayEncoding encoding, const CudaBackend::Texture2D& src,
                             CudaBackend::Texture2D& dst, std::uint32_t width,
                             std::uint32_t height) {
  constexpr const char* kCaller = "RecordCudaFilmGrainPost";
  const auto field = cuda_film_grain::FieldParamsFor(plan, geometry, width, height, kCaller);
  if (src.Width() != width || src.Height() != height || dst.Width() != width ||
      dst.Height() != height) {
    throw std::invalid_argument("RecordCudaFilmGrainPost: the images do not have the render size");
  }
  const FilmGrainPostParams post =
      MakeFilmGrainPostParams(plan, encoding.transfer, encoding.decode_scale);
  cuda_film_grain::Launch(
      device.CommandContext().Stream(),
      PostApply{static_cast<const float4*>(src.DevicePointer()),
                static_cast<float4*>(dst.DevicePointer()), static_cast<int>(width)},
      post, field, width, height, kCaller);
}

void RecordCudaFilmGrainNegative(CudaRenderDevice& device, const FilmGrainPlan& plan,
                                 const ResolvedRenderGeometry& geometry,
                                 std::span<const double> reversal_dye_max, float* amounts,
                                 std::uint32_t width, std::uint32_t height) {
  constexpr const char* kCaller = "RecordCudaFilmGrainNegative";
  const auto field = cuda_film_grain::FieldParamsFor(plan, geometry, width, height, kCaller);
  if (amounts == nullptr) {
    throw std::invalid_argument("RecordCudaFilmGrainNegative: the amounts are null");
  }
  const FilmGrainNegativeParams negative = MakeFilmGrainNegativeParams(plan, reversal_dye_max);
  cuda_film_grain::Launch(
      device.CommandContext().Stream(),
      NegativeApply{reinterpret_cast<float3*>(amounts), static_cast<int>(width)}, negative, field,
      width, height, kCaller);
}

}  // namespace alcedo
