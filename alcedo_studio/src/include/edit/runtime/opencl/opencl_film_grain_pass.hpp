//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <span>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"

namespace alcedo {

/**
 * @brief Record the DRT/Post Film Grain of ACES 2.0 and OpenDRT: @p src to @p dst, both display
 *        images of @p width x @p height (RGBA float, an image or a scene work buffer).
 *
 * The steps of `RecordCudaFilmGrainPost` in one kernel (film_grain_post.cl): each 16 x 16
 * work-group makes the noise of the reference pixels of its tile with a halo of the largest blur
 * radius in local memory, one band of rows at a time, then the horizontal blur, the vertical blur,
 * the division by the sum of w^2, the mix, and the apply step of film_grain_post.h. The fixed
 * values are one upload in the transient arena of the render, because they are larger than the
 * by-value arguments of OpenCL 1.2 allow. The pass allocates no image.
 *
 * @p src and @p dst must be different storage: the kernel reads and writes through separate
 * OpenCL arguments. Does not wait for the GPU. A disabled plan (strength 0) is a caller error: the
 * DRT/Post schedule does not run it.
 *
 * @throws std::invalid_argument for a disabled plan, a plan that is not a Post plan, a size that
 *         is not the render size, or a blur radius above 24, and std::runtime_error for a program,
 *         allocation, upload, argument, or enqueue failure. There is no change of backend.
 */
void RecordOpenClFilmGrainPost(OpenClRenderDevice& device, const FilmGrainPlan& plan,
                               const ResolvedRenderGeometry& geometry,
                               FilmGrainDisplayEncoding encoding, const OpenClSceneView& src,
                               const OpenClSceneView& dst, std::uint32_t width,
                               std::uint32_t height);

/**
 * @brief The fixed values of the grain in the negative of one render (phase G6), uploaded, and the
 *        launch size of a kernel with the steps of film_grain_tile.h.
 *
 * A kernel takes @ref data, @ref field_offset, and @ref negative_offset as three arguments, then a
 * local memory argument of @ref local_bytes, and runs with work-groups of 16 x 16 over
 * @ref global (each lane covers two pixels of a 16 x 32 tile).
 */
struct OpenClFilmGrainNegative {
  cl_mem      data            = nullptr;
  cl_uint     field_offset    = 0;  ///< In bytes from the start of @ref data
  cl_uint     negative_offset = 0;  ///< In bytes from the start of @ref data
  std::size_t local_bytes     = 0;
  std::size_t global[2]       = {0, 0};
};

/**
 * @brief Upload the field and the apply values of the grain in the negative of @p plan for a
 *        render of @p width x @p height, in the transient arena of the render.
 *
 * @param reversal_dye_max The `dye_max` of a colour reversal film, or an empty span for a
 *        negative (`MakeFilmGrainNegativeParams`).
 * @throws std::invalid_argument that starts with @p caller for a disabled plan, a plan that is not
 *         a Negative plan, an empty render, a blur radius outside 0 to 24, or dye_max values that
 *         do not match the film; std::runtime_error for an allocation or upload failure.
 */
[[nodiscard]] auto UploadOpenClFilmGrainNegative(OpenClRenderDevice&           device,
                                                 const FilmGrainPlan&          plan,
                                                 const ResolvedRenderGeometry& geometry,
                                                 std::span<const double>       reversal_dye_max,
                                                 std::uint32_t width, std::uint32_t height,
                                                 const char* caller) -> OpenClFilmGrainNegative;

/**
 * @brief Record the grain in the negative of the film print alone (phase G6): add the grain of
 *        @p plan to the dye amounts of @p width x @p height pixels in @p amounts, in place.
 *
 * @p amounts holds three packed floats per pixel from @p amounts_offset (in floats), row by row:
 * the record of each layer after the inhibitor step. The film print runs the same steps inside its
 * FP6 kernels and writes no amounts with grain; this function gives the step alone, so the backend
 * tests measure the dye amounts of each stock, as `RecordCudaFilmGrainNegative`. Does not wait for
 * the GPU.
 *
 * @throws std::invalid_argument as @ref UploadOpenClFilmGrainNegative, and for null amounts;
 *         std::runtime_error for a program, argument, or enqueue failure.
 */
void RecordOpenClFilmGrainNegative(OpenClRenderDevice& device, const FilmGrainPlan& plan,
                                   const ResolvedRenderGeometry& geometry,
                                   std::span<const double> reversal_dye_max, cl_mem amounts,
                                   cl_uint amounts_offset, std::uint32_t width,
                                   std::uint32_t height);

}  // namespace alcedo

#endif  // HAVE_OPENCL
