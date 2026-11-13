//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo {

/**
 * @brief Record the DRT/Post Film Grain of ACES 2.0 and OpenDRT: @p src to @p dst, both display
 * images of @p width x @p height (RGBA float).
 *
 * The steps of section 6.3 of the plan 'film_grain_format_and_stock_plan.md', in one kernel: each
 * 16 x 16 tile makes the noise of its reference pixels with a halo of the largest blur radius
 * (rule 'grain_aggregate') in shared memory, one band of rows at a time (the layout of the OpenCL
 * and Metal kernels), then the horizontal blur, the vertical blur, the
 * division by the sum of w^2, the mix, and the apply step of film_grain_post.h. The halo makes a
 * viewport region equal to the same region of the full frame. The pass uses no pool image, so it
 * allocates no GPU memory.
 *
 * @p dst can be @p src: the last pass reads and writes the same pixel only. Does not wait for the
 * GPU. A disabled plan (strength 0) is a caller error: the DRT/Post schedule does not run it.
 *
 * @throws std::invalid_argument for a disabled plan or a plan that is not a Post plan, and
 *         std::runtime_error for a kernel launch failure.
 */
void RecordCudaFilmGrainPost(CudaRenderDevice& device, const FilmGrainPlan& plan,
                             const ResolvedRenderGeometry& geometry,
                             FilmGrainDisplayEncoding encoding, const CudaBackend::Texture2D& src,
                             CudaBackend::Texture2D& dst, std::uint32_t width,
                             std::uint32_t height);

/**
 * @brief Record the grain in the negative of the film print (phase G5): add the grain of @p plan
 * to the dye amounts @p amounts of @p width x @p height pixels, in place.
 *
 * @p amounts holds three packed floats per pixel, row by row: the record of each layer after the
 * inhibitor step, as the images between the steps of the film print. The kernel is the grain tile
 * kernel (cuda_film_grain_tile.cuh) with the apply step of film_grain_negative.h, so a viewport
 * region gets the grain of the same region of the full frame. The film print runs the same apply
 * step inside its FP6 kernel and writes no amounts with grain; this function gives the step alone,
 * so the backend tests measure the dye amounts of each stock (phases G5 and G6). Does not wait for
 * the GPU.
 *
 * @param reversal_dye_max The `dye_max` of a colour reversal film, or an empty span for a
 *        negative (`MakeFilmGrainNegativeParams`).
 * @throws std::invalid_argument for a disabled plan, a plan that is not a Negative plan, an empty
 *         render, null amounts, or dye_max values that do not match the film; std::runtime_error
 *         for a kernel launch failure.
 */
void RecordCudaFilmGrainNegative(CudaRenderDevice& device, const FilmGrainPlan& plan,
                                 const ResolvedRenderGeometry& geometry,
                                 std::span<const double> reversal_dye_max, float* amounts,
                                 std::uint32_t width, std::uint32_t height);

}  // namespace alcedo
