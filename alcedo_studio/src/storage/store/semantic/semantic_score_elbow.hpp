//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Private to the semantic stores: the score cutoff shared by vector search (over many files)
// and label assignment (over a few prototypes).

#pragma once

#include <span>

namespace alcedo::semantic_score_elbow {

/**
 * @brief Cutoff score of a descending score sequence: every item at or above it is kept.
 *
 * Keeps everything for at most two scores or a flat sequence. Otherwise cuts at the largest
 * gap when it is clearly larger than the others, and else keeps the top part of the score span.
 * Only the first 256 scores are sampled.
 */
[[nodiscard]] auto CutoffScore(std::span<const double> scores_desc) -> double;

}  // namespace alcedo::semantic_score_elbow
