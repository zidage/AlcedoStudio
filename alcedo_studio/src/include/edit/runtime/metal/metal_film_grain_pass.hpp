//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>

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

}  // namespace alcedo

#endif  // HAVE_METAL
