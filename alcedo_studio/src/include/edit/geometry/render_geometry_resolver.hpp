//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/geometry/render_request.hpp"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/source_geometry.hpp"

namespace alcedo {

/**
 * @brief Resolves crop, straightening, orientation, view ROI, and scale into matrices and rects.
 *
 * Does not process pixels or allocate GPU memory. All integer rounding for image, mask,
 * and LLF happens here.
 *
 * @param source Decoded and full-reference extents. @p decoded_to_reference is rebuilt
 *        from those extents.
 * @param image Crop and straightening, output frame, and exact orientation of that frame.
 * @param view Visible EditSpace rect and optional viewport size.
 * @param resolution Render scale, max edge, and filter quality.
 * @param footprint Source-pixel neighborhood. @p requires_full_reference forces full decoded
 *        and reference required rects (LLF).
 *
 * @throws std::runtime_error for invalid input or a singular mapping.
 *
 * Not thread-safe only in the sense that it has no shared mutable state; the function is pure.
 */
[[nodiscard]] auto ResolveRenderGeometry(const SourceGeometry&     source,
                                         const ImageGeometryParams& image,
                                         const ViewRequest&         view,
                                         const ResolutionRequest&   resolution,
                                         const SamplingFootprint&   footprint)
    -> ResolvedRenderGeometry;

}  // namespace alcedo
