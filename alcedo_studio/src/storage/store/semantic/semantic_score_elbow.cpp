//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "semantic_score_elbow.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace alcedo::semantic_score_elbow {
namespace {
constexpr size_t kCutoffSampleLimit          = 256;
constexpr double kFlatScoreSpanRatio         = 0.08;
constexpr double kFallbackScoreSpanKeepRatio = 0.35;
constexpr double kElbowGapToSpanRatio        = 0.18;
constexpr double kElbowGapToMedianRatio      = 3.0;
}  // namespace

auto CutoffScore(std::span<const double> scores_desc) -> double {
  if (scores_desc.empty()) {
    return 0.0;
  }
  if (scores_desc.size() <= 2) {
    return scores_desc.back();
  }

  const auto   sample_count = std::min(scores_desc.size(), kCutoffSampleLimit);
  const double top_score    = scores_desc.front();
  const double tail_score   = scores_desc[sample_count - 1];
  const double score_span   = top_score - tail_score;
  if (score_span <= std::abs(top_score) * kFlatScoreSpanRatio) {
    return tail_score;
  }

  std::vector<double> gaps;
  gaps.reserve(sample_count - 1);
  double best_gap   = 0.0;
  size_t best_index = 0;
  for (size_t i = 0; i + 1 < sample_count; ++i) {
    const double gap = scores_desc[i] - scores_desc[i + 1];
    gaps.push_back(gap);
    if (gap > best_gap) {
      best_gap   = gap;
      best_index = i;
    }
  }

  auto sorted_gaps = gaps;
  std::sort(sorted_gaps.begin(), sorted_gaps.end());
  const double median_gap      = sorted_gaps[sorted_gaps.size() / 2];
  const bool   has_clear_elbow = best_gap >= score_span * kElbowGapToSpanRatio &&
                               best_gap >= median_gap * kElbowGapToMedianRatio;
  if (has_clear_elbow) {
    return (scores_desc[best_index] + scores_desc[best_index + 1]) / 2.0;
  }

  return top_score - (score_span * kFallbackScoreSpanKeepRatio);
}

}  // namespace alcedo::semantic_score_elbow
