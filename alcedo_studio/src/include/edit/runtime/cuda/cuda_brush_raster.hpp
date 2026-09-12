//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/mask/brush_coverage_update.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"

namespace alcedo {

/**
 * @brief Stamp or regionally replay a parameterized Brush onto an existing GPU R8.
 *
 * CPU owns commands. This writes canonical coverage texels in evaluation order:
 * paint is max, erase is min(previous, 255-dab). @c StampNewSamples does not zero
 * texels. @c ReplayDirty zeros @c update.dirty then restamps intersecting dabs.
 * Uninitialized textures must pass @p texture_uninitialized true so the first
 * write starts from zeros. There is no CPU raster substitute.
 *
 * @throws std::runtime_error on unsupported algorithm versions or CUDA launch
 *         failure, or std::invalid_argument on empty geometry.
 *
 * Thread: CUDA Mask pass on the render thread. Uses @p device's current stream.
 */
void ApplyParameterizedBrushCuda(CudaRenderDevice& device, CudaBackend::Texture2D& dest,
                                 const BrushMaskSource& source, Extent2D full_reference,
                                 const BrushCoverageUpdate& update, bool texture_uninitialized);

}  // namespace alcedo
