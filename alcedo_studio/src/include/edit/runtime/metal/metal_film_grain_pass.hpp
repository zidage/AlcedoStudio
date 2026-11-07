//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>
#include <span>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Record the DRT/Post Film Grain of ACES 2.0 and OpenDRT: @p src to @p dst, both display
 *        textures of @p width x @p height (RGBA float).
 *
 * The steps of `RecordCudaFilmGrainPost` in one kernel (film_grain_post.metal of the DRT library):
 * each 16 x 16 threadgroup makes the noise of the reference pixels of its tile with a halo of the
 * largest blur radius in threadgroup memory, one band of rows at a time, then the horizontal blur,
 * the vertical blur, the division by the sum of w^2, the mix, and the apply step of
 * film_grain_post.h. The fixed values go to the kernel with `setBytes`. The pass allocates no
 * GPU memory.
 *
 * @p src and @p dst must be different textures. Does not wait for the GPU. A disabled plan
 * (strength 0) is a caller error: the DRT/Post schedule does not run it.
 *
 * @throws std::invalid_argument for a disabled plan, a plan that is not a Post plan, a size that
 *         is not the render size, or a blur radius above 24, and std::runtime_error for a missing
 *         pipeline, a pipeline with fewer than 256 threads to a threadgroup, or a missing encoder.
 *         There is no change of backend.
 */
void RecordMetalFilmGrainPost(MetalRenderDevice& device, const FilmGrainPlan& plan,
                              const ResolvedRenderGeometry& geometry,
                              FilmGrainDisplayEncoding encoding, const MetalBackend::Texture2D& src,
                              MetalBackend::Texture2D& dst, std::uint32_t width,
                              std::uint32_t height);

/**
 * @brief The fixed values of the grain in the negative of one render (phase G6) and the launch
 *        size of a kernel with the steps of film_grain_tile.h.
 *
 * A kernel takes @ref field and @ref negative as `constant` arguments (`setBytes`) and
 * @ref threadgroup_bytes of threadgroup memory at index 0, and runs @ref groups threadgroups of
 * 16 x 16 threads (each thread covers two pixels of a 16 x 32 tile).
 */
struct MetalFilmGrainNegative {
  FilmGrainFieldParams    field{};
  FilmGrainNegativeParams negative{};
  std::size_t             threadgroup_bytes = 0;
  std::uint32_t           groups[2]         = {0, 0};
};

/**
 * @brief The field and the apply values of the grain in the negative of @p plan for a render of
 *        @p width x @p height.
 *
 * @param reversal_dye_max The `dye_max` of a colour reversal film, or an empty span for a
 *        negative (`MakeFilmGrainNegativeParams`).
 * @throws std::invalid_argument that starts with @p caller for a disabled plan, a plan that is not
 *         a Negative plan, an empty render, a blur radius outside 0 to 24, or dye_max values that
 *         do not match the film.
 */
[[nodiscard]] auto MakeMetalFilmGrainNegative(const FilmGrainPlan&          plan,
                                              const ResolvedRenderGeometry& geometry,
                                              std::span<const double>       reversal_dye_max,
                                              std::uint32_t width, std::uint32_t height,
                                              const char* caller) -> MetalFilmGrainNegative;

/**
 * @brief Record the grain in the negative of the film print alone (phase G6): add the grain of
 *        @p plan to the dye amounts of @p width x @p height pixels in @p amounts, in place.
 *
 * @p amounts is an MTL::Buffer with packed_float3 per pixel from @p offset bytes, row by row: the
 * record of each layer after the inhibitor step. The film print runs the same steps inside its FP6
 * kernels (film_print.metal) and writes no amounts with grain; this function gives the step alone,
 * so the backend tests measure the dye amounts of each stock, as `RecordCudaFilmGrainNegative`.
 * The kernel is in film_print.metal, so it has the compile options of the FP6 kernels. Does not
 * wait for the GPU.
 *
 * @throws std::invalid_argument as @ref MakeMetalFilmGrainNegative, and for null amounts;
 *         std::runtime_error for a missing pipeline, a pipeline below 256 threads to a
 *         threadgroup, or a missing compute encoder.
 */
void RecordMetalFilmGrainNegative(MetalRenderDevice& device, const FilmGrainPlan& plan,
                                  const ResolvedRenderGeometry& geometry,
                                  std::span<const double> reversal_dye_max, void* amounts,
                                  std::size_t offset, std::uint32_t width, std::uint32_t height);

}  // namespace alcedo

#endif  // HAVE_METAL
