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
  float         black[3]       = {};
  float         scale[3]       = {1.0f, 1.0f, 1.0f};
  std::uint32_t integer_codes  = 0;
  /// When 1, each channel is clamped to [0, range_max] after the scale. The AI Denoise input sets
  /// it to keep the clamp of the CFA Linearize step (DevelopRgbLinearization); decoded RGB keeps
  /// its range and leaves it 0.
  std::uint32_t clamp_to_range = 0;
  float         range_max[3]   = {1.0f, 1.0f, 1.0f};
};

static_assert(sizeof(RawRgbLinearizationParams) == 44);

}  // namespace alcedo
