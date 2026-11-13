//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Shared last step of the RAW and raster input loaders. Not a public API.

#pragma once

#include <cstdint>

#include "edit/input/prepared_raw_input.hpp"

namespace alcedo::input_detail {

/**
 * @brief Mark @p input as RasterRgb and fill its output geometry and source key.
 *
 * The caller has set the host plane, host extent, downsample passes and sensor geometry
 * (raw size = full-resolution image size, orientation flip).
 */
[[nodiscard]] auto FinishRasterPrepared(PreparedRawInput input, std::uint64_t encoded_hash,
                                        std::uint64_t encoded_byte_count) -> PreparedRawInput;

}  // namespace alcedo::input_detail
