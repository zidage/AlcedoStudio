//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/mask_coverage_selection.hpp"

namespace alcedo {

/**
 * @brief The Mask candidate of a new AI Mask edit and the ranges set during that edit (#604).
 *
 * The candidate is not a Mask of the document yet. Its range coverage is the raster MaskEvaluate
 * of @p selection with the ranges on the Grade input, with the invert and opacity of a new Mask
 * (false, 1): the coverage that the Mask has after the commit.
 */
struct MaskCoverageProbeCandidate {
  MaskCoverageCandidateSelection     selection;
  std::optional<LuminanceRangeModel> luminance_range;
  std::optional<ColorRangeModel>     color_range;

  auto operator==(const MaskCoverageProbeCandidate&) const -> bool = default;
};

/// The Mask fields of @p candidate that MaskEvaluate reads: its ranges, no invert, opacity 1.
[[nodiscard]] inline auto ProbeCandidateMaskModel(const MaskCoverageProbeCandidate& candidate)
    -> MaskModel {
  MaskModel mask;
  mask.luminance_range = candidate.luminance_range;
  mask.color_range     = candidate.color_range;
  return mask;
}

/**
 * @brief The Mask whose coverage an interactive photo render also shows in the viewport (Mask
 *        ranges plan, D8 and D9).
 *
 * The coverage is the R8 `effective_output` that MaskEvaluate writes for Mask @p mask_id of Color
 * Grade @p grade_id in the same render: source, Luminance Range and Hue Range on the Grade input,
 * invert, opacity. With @p candidate, @p mask_id is empty and the render evaluates the candidate
 * of a new AI Mask on the input of Grade @p grade_id for the display only. It is presentation
 * state of one render request. It is not part of the document, the plan, or any result key, so it
 * changes no photo pixel, scope input, cache entry, or history entry.
 */
struct MaskCoverageProbe {
  NodeId                                    grade_id;
  MaskId                                    mask_id;
  std::optional<MaskCoverageProbeCandidate> candidate;

  auto operator==(const MaskCoverageProbe& other) const -> bool = default;
};

/// What a render with a @ref MaskCoverageProbe did with the coverage channel.
enum class MaskCoverageProbeOutcome : std::uint8_t {
  /// The render request has no probe.
  NotRequested = 0,
  /// The coverage frame waits for the viewport and replaces the displayed coverage.
  Published,
  /// The render did not evaluate the probed Mask (the Mask or its Grade is disabled, removed, or
  /// not in the plan). The coverage channel was cleared, so no older coverage stays displayed.
  NotEvaluated,
  /// The viewport changed to another image session first; nothing was published.
  DiscardedForImageChange,
};

}  // namespace alcedo
