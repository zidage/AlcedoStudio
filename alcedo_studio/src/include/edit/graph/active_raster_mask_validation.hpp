//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>

#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/active_raster_mask.hpp"

namespace alcedo {

/**
 * @brief Reject active raster inputs that do not target an existing Brush Mask.
 *
 * Also runs @ref ValidateActiveRasterMaskFields. Does not upload or mutate assets.
 *
 * @throws std::runtime_error when a Grade, Mask, source kind, or descriptor is invalid.
 */
void ValidateActiveRasterMaskBindings(const PipelineDocument& document,
                                      std::span<const ActiveRasterMaskInput> inputs);

}  // namespace alcedo
