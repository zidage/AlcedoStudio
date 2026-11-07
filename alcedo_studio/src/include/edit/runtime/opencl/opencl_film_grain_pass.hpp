//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>

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

}  // namespace alcedo

#endif  // HAVE_OPENCL
