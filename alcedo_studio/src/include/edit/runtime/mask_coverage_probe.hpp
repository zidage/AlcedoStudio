//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_id.hpp"

namespace alcedo {

/**
 * @brief The Mask whose coverage an interactive photo render also shows in the viewport (Mask
 *        ranges plan, D8 and D9).
 *
 * The coverage is the R8 `effective_output` that MaskEvaluate writes for Mask @p mask_id of Color
 * Grade @p grade_id in the same render: source, Luminance Range and Hue Range on the Grade input,
 * invert, opacity. It is presentation state of one render request. It is not part of the
 * document, the plan, or any result key, so it changes no photo pixel, scope input, cache entry,
 * or history entry.
 */
struct MaskCoverageProbe {
  NodeId grade_id;
  MaskId mask_id;

  auto   operator==(const MaskCoverageProbe& other) const -> bool = default;
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
