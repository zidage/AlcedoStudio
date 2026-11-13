//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>

#include "app/mask_thumbnail_spec.hpp"

namespace alcedo {

/**
 * @brief Render one 128×128 grayscale8 Mask thumbnail from @p spec.
 *
 * Letterboxes the resolver's content rect on a black canvas. Invert and opacity
 * apply only inside the photograph; letterbox and out-of-photo samples stay 0.
 * Group layers combine with per-pixel maximum. Thread-safe: no shared state.
 *
 * @throws std::runtime_error when geometry cannot be resolved or evaluation fails.
 */
[[nodiscard]] auto RenderMaskThumbnail(const MaskThumbnailSpec& spec) -> QImage;

}  // namespace alcedo
