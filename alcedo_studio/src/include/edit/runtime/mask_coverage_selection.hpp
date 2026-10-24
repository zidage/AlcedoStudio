//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <variant>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"

namespace alcedo {

/// The AI Mask candidate of the edit session: its shared original coverage and edge values.
struct MaskCoverageCandidateSelection {
  AiMaskCoveragePixels pixels{};
  Extent2D             analysis_extent{};
  AiMaskEdgeAdjustment edge{};
};

/// The coverage that one presentation displays: the candidate or one committed AI Mask (SM8 C8).
using MaskCoverageSelection =
    std::variant<MaskCoverageCandidateSelection, AiSegmentationMaskSource>;

/// The Mask fields that change effective coverage: those of the Mask (a new Mask uses false, 1).
struct MaskCoverageInvertOpacity {
  bool  invert  = false;
  float opacity = 1.0f;
};

}  // namespace alcedo
