//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Black/white and as-shot cam_mul used by GPU linearization. No LibRaw types.
 *
 * @p apply_as_shot_wb is true when the file has not already applied camera white
 * balance, matching the absence of LibRaw's LIBRAW_ASWB_APPLIED bit.
 */
struct RawLinearizationParams {
  float         black_level[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
  float         white_level[4]     = {1.0f, 1.0f, 1.0f, 1.0f};
  float         cam_mul[4]         = {1.0f, 1.0f, 1.0f, 1.0f};
  std::int32_t  apply_as_shot_wb   = 1;
  std::int32_t  black_tile_width   = 0;
  std::int32_t  black_tile_height  = 0;
  float         pattern_black[36]  = {};
  /// LibRaw `color.maximum` of the unpacked file (one value for all colors): the white level of
  /// the AI Denoise input preparation (AI Denoise integration plan, DN6; lab
  /// docs/alcedo_integration.md). 0 when the input does not come from LibRaw. The GPU passes use
  /// `white_level`.
  float         libraw_maximum     = 0.0f;
};

}  // namespace alcedo
