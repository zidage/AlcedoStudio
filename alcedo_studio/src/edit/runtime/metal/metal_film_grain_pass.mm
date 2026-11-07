//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_film_grain_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <alcedo/metal/Metal.hpp>

#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_post.h"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

// film_grain_post.metal has the same sizes; a difference stops the build of the DRT library.
static_assert(sizeof(FilmGrainFieldParams) == 388);
static_assert(sizeof(FilmGrainPostParams) == 612);

/// The side of the tile of one threadgroup: FILM_GRAIN_POST_TILE of film_grain_post.metal.
constexpr std::uint32_t kTile = 16;

/// `FilmGrainPostSize` of film_grain_post.metal.
struct FilmGrainPostSize {
  std::uint32_t width;
  std::uint32_t height;
};

/// Threadgroup memory of one threadgroup: the noise band and the horizontal blur, 3 channels of
/// kTile x (kTile + 2 halo) floats each. Metal takes a multiple of 16 bytes.
auto ThreadgroupBytes(int halo) -> NS::UInteger {
  const auto band = static_cast<std::size_t>(kTile) * (kTile + 2U * static_cast<std::size_t>(halo));
  const auto bytes = 2U * 3U * band * sizeof(float);
  return static_cast<NS::UInteger>((bytes + 15U) / 16U * 16U);
}

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
  encoder->setThreadgroupMemoryLength(ThreadgroupBytes(field.halo), 0);
  encoder->dispatchThreadgroups(
      MTL::Size{(width + kTile - 1) / kTile, (height + kTile - 1) / kTile, 1},
      MTL::Size{kTile, kTile, 1});
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
#endif
}

}  // namespace alcedo
