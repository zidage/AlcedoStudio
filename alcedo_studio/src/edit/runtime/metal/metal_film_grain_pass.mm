//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_film_grain_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <alcedo/metal/Metal.hpp>

#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_negative.h"
#include "edit/runtime/film/film_grain_post.h"
#include "edit/runtime/film/film_grain_tile.h"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

// film_grain_post.metal has the same sizes; a difference stops the build of the DRT library.
static_assert(sizeof(FilmGrainFieldParams) == 388);
static_assert(sizeof(FilmGrainPostParams) == 612);
static_assert(sizeof(FilmGrainNegativeParams) == 616);

/// The side of the tile of one threadgroup: FILM_GRAIN_TILE of film_grain_tile.h.
constexpr std::uint32_t kTile = FILM_GRAIN_TILE;

/// `FilmGrainPostSize` of film_grain_post.metal.
struct FilmGrainPostSize {
  std::uint32_t width;
  std::uint32_t height;
};

/// Threadgroup memory of one threadgroup: the noise band and the horizontal blur of
/// film_grain_tile.h. Metal takes a multiple of 16 bytes.
auto ThreadgroupBytes(int halo, int tile_rows) -> std::size_t {
  const auto bytes =
      static_cast<std::size_t>(FilmGrainTileSharedFloats(halo, tile_rows)) * sizeof(float);
  return (bytes + 15U) / 16U * 16U;
}

/// `FilmSize` of film_print.metal.
struct FilmSize {
  std::int32_t width;
  std::int32_t height;
};

}  // namespace

void RecordMetalFilmGrainPost(MetalRenderDevice& device, const FilmGrainPlan& plan,
                              const ResolvedRenderGeometry& geometry,
                              FilmGrainDisplayEncoding encoding, const MetalBackend::Texture2D& src,
                              MetalBackend::Texture2D& dst, std::uint32_t width,
                              std::uint32_t height) {
  if (!plan.enabled) {
    throw std::invalid_argument("RecordMetalFilmGrainPost: the grain plan is disabled");
  }
  if (width == 0 || height == 0 || src.Width() != width || src.Height() != height ||
      dst.Width() != width || dst.Height() != height) {
    throw std::invalid_argument("RecordMetalFilmGrainPost: the images do not have the render size");
  }
  if (src.Native() == dst.Native()) {
    throw std::invalid_argument("RecordMetalFilmGrainPost: the source is the destination");
  }
  const FilmGrainFieldParams field = MakeFilmGrainFieldParams(plan, geometry);
  const FilmGrainPostParams  post =
      MakeFilmGrainPostParams(plan, encoding.transfer, encoding.decode_scale);
  if (field.halo < 0 || field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument("RecordMetalFilmGrainPost: the blur radius is outside 0 to 24");
  }
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
  throw std::runtime_error(
      "RecordMetalFilmGrainPost: the Metal DRT metallib path is not configured");
#else
  auto pipeline = metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DRT_METALLIB_PATH, "film_grain_post", "Metal DRT Post Film Grain");
  if (!pipeline) {
    throw std::runtime_error("RecordMetalFilmGrainPost: no pipeline state for film_grain_post");
  }
  if (pipeline->maxTotalThreadsPerThreadgroup() < kTile * kTile) {
    throw std::runtime_error(
        "RecordMetalFilmGrainPost: the pipeline allows fewer than 256 threads to a threadgroup");
  }
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("RecordMetalFilmGrainPost: compute encoder is missing");
  }
  const FilmGrainPostSize size{width, height};
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBytes(&field, sizeof(field), 0);
  encoder->setBytes(&post, sizeof(post), 1);
  encoder->setBytes(&size, sizeof(size), 2);
  encoder->setThreadgroupMemoryLength(ThreadgroupBytes(field.halo, FILM_GRAIN_POST_TILE_ROWS), 0);
  encoder->dispatchThreadgroups(
      MTL::Size{(width + kTile - 1) / kTile, (height + kTile - 1) / kTile, 1},
      MTL::Size{kTile, kTile, 1});
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
#endif
}

auto MakeMetalFilmGrainNegative(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                                std::span<const double> reversal_dye_max, std::uint32_t width,
                                std::uint32_t height, const char* caller)
    -> MetalFilmGrainNegative {
  const std::string prefix(caller);
  if (!plan.enabled) {
    throw std::invalid_argument(prefix + ": the grain plan is disabled");
  }
  if (width == 0 || height == 0) {
    throw std::invalid_argument(prefix + ": the render is empty");
  }
  MetalFilmGrainNegative grain;
  grain.field = MakeFilmGrainFieldParams(plan, geometry);
  if (grain.field.halo < 0 || grain.field.halo > FILM_GRAIN_MAX_BLUR_RADIUS) {
    throw std::invalid_argument(prefix + ": the blur radius is outside 0 to 24");
  }
  grain.negative          = MakeFilmGrainNegativeParams(plan, reversal_dye_max);
  constexpr auto kRows    = static_cast<std::uint32_t>(FILM_GRAIN_NEGATIVE_TILE_ROWS);
  grain.threadgroup_bytes = ThreadgroupBytes(grain.field.halo, FILM_GRAIN_NEGATIVE_TILE_ROWS);
  grain.groups[0]         = (width + kTile - 1) / kTile;
  grain.groups[1]         = (height + kRows - 1) / kRows;
  return grain;
}

void RecordMetalFilmGrainNegative(MetalRenderDevice& device, const FilmGrainPlan& plan,
                                  const ResolvedRenderGeometry& geometry,
                                  std::span<const double> reversal_dye_max, void* amounts,
                                  std::size_t offset, std::uint32_t width, std::uint32_t height) {
  const auto grain = MakeMetalFilmGrainNegative(plan, geometry, reversal_dye_max, width, height,
                                                "RecordMetalFilmGrainNegative");
  if (amounts == nullptr) {
    throw std::invalid_argument("RecordMetalFilmGrainNegative: the amounts are null");
  }
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
  (void)device;
  (void)offset;
  throw std::runtime_error(
      "RecordMetalFilmGrainNegative: the Metal DRT metallib path is not configured");
#else
  auto pipeline = metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_DRT_METALLIB_PATH, "film_grain_negative_amounts",
      "Metal film print grain in the negative");
  if (!pipeline) {
    throw std::runtime_error(
        "RecordMetalFilmGrainNegative: no pipeline state for film_grain_negative_amounts");
  }
  if (pipeline->maxTotalThreadsPerThreadgroup() < kTile * kTile) {
    throw std::runtime_error("RecordMetalFilmGrainNegative: the pipeline allows fewer than 256 "
                             "threads to a threadgroup");
  }
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("RecordMetalFilmGrainNegative: compute encoder is missing");
  }
  const FilmSize size{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
  encoder->setComputePipelineState(pipeline.get());
  encoder->setBuffer(static_cast<MTL::Buffer*>(amounts), offset, 0);
  encoder->setBytes(&grain.field, sizeof(grain.field), 1);
  encoder->setBytes(&grain.negative, sizeof(grain.negative), 2);
  encoder->setBytes(&size, sizeof(size), 3);
  encoder->setThreadgroupMemoryLength(grain.threadgroup_bytes, 0);
  encoder->dispatchThreadgroups(MTL::Size{grain.groups[0], grain.groups[1], 1},
                                MTL::Size{kTile, kTile, 1});
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
#endif
}

}  // namespace alcedo
