//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Convert unpacked RGB codes to white-balanced linear RGB on the GPU.
 *
 * scale includes normalization and as-shot gains only when not already applied.
 * The normal RAW pack stage removes those gains after highlight reconstruction,
 * producing unbalanced camera RGB for the later camera color transform.
 * Float inputs retain their range and negative values; integer codes clamp at black.
 * Scalar arrays keep the ABI identical in CUDA, OpenCL and Metal.
 */
struct RawRgbLinearizationParams {
  float         black[3]      = {};
  float         scale[3]      = {1.0f, 1.0f, 1.0f};
  std::uint32_t integer_codes = 0;
};

static_assert(sizeof(RawRgbLinearizationParams) == 28);

}  // namespace alcedo
