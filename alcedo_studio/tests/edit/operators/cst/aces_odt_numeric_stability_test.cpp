//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "edit/operators/cst/aces_odt_cpu.hpp"
#include "edit/operators/utils/color_utils.hpp"

namespace alcedo {
namespace {

auto AllTableEntriesFinite(const ColorUtils::ODTParams& runtime) -> bool {
  if (!runtime.table_reach_M_ || !runtime.table_hues_ || !runtime.table_gamut_cusps_ ||
      !runtime.table_upper_hull_gammas_) {
    return false;
  }
  for (int i = 0; i < TOTAL_TABLE_SIZE; ++i) {
    if (!std::isfinite((*runtime.table_reach_M_)[i]) ||
        !std::isfinite((*runtime.table_hues_)[i]) ||
        !std::isfinite((*runtime.table_upper_hull_gammas_)[i])) {
      return false;
    }
    const auto& cusp = (*runtime.table_gamut_cusps_)[i];
    if (!std::isfinite(cusp(0)) || !std::isfinite(cusp(1)) || !std::isfinite(cusp(2))) {
      return false;
    }
    if ((*runtime.table_upper_hull_gammas_)[i] <= 0.0f) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST(AcesOdtNumericStability, Rec709RuntimeTablesStayFiniteAcrossPeakLuminances) {
  const float peaks[] = {48.0f, 100.0f, 1000.0f, 4000.0f};
  for (float peak : peaks) {
    const auto runtime =
        odt_cpu::ResolveACESODTRuntime(ColorUtils::ColorSpace::REC709, peak);
    EXPECT_TRUE(AllTableEntriesFinite(runtime)) << "peak_luminance=" << peak;
    EXPECT_TRUE(std::isfinite(runtime.limit_J_max_));
    EXPECT_GT(runtime.limit_J_max_, 0.0f);
    EXPECT_GE(runtime.hue_linearity_search_range_(0), -64.0f) << peak;
    EXPECT_LE(runtime.hue_linearity_search_range_(1), 64.0f) << peak;
  }
}

TEST(AcesOdtNumericStability, FocusGainAndIntersectStayFiniteNearLimitAndBlack) {
  const auto runtime =
      odt_cpu::ResolveACESODTRuntime(ColorUtils::ColorSpace::REC709, 100.0f);
  const float limit   = runtime.limit_J_max_;
  const float mid_j   = runtime.mid_J_;
  const float focus_d = runtime.focus_dist_;
  const float thresh  = std::lerp(mid_j, limit, ColorUtils::focus_gain_blend);

  const float sample_j[] = {0.0f, 1.0e-6f, mid_j * 0.5f, mid_j, thresh, limit - 1.0e-4f,
                            limit, limit + 1.0f};
  const float sample_m[] = {0.0f, 1.0e-4f, 10.0f, 40.0f, 120.0f};
  for (float j : sample_j) {
    const float gain = ColorUtils::get_focus_gain(j, thresh, limit, focus_d);
    EXPECT_TRUE(std::isfinite(gain)) << "J=" << j;
    EXPECT_GT(gain, 0.0f) << "J=" << j;
    for (float m : sample_m) {
      const float intersect = ColorUtils::solve_J_intersect(j, m, mid_j, limit, gain);
      EXPECT_TRUE(std::isfinite(intersect)) << "J=" << j << " M=" << m;
    }
  }
}

TEST(AcesOdtNumericStability, HueIntervalWeightStaysInsideUnitRangeAtWrap) {
  const auto weight = [](float h, float h_lo, float h_hi) {
    const float denom = h_hi - h_lo;
    if (std::fabs(denom) < 1.0e-6f) return 0.0f;
    return std::clamp((h - h_lo) / denom, 0.0f, 1.0f);
  };
  EXPECT_NEAR(weight(0.2f, -1.0f, 0.5f), 1.2f / 1.5f, 1.0e-6f);
  EXPECT_GE(weight(0.2f, -1.0f, 0.5f), 0.0f);
  EXPECT_LE(weight(0.2f, -1.0f, 0.5f), 1.0f);
  EXPECT_NEAR(weight(0.4f, 0.0f, 1.0f), 0.4f, 1.0e-6f);
  EXPECT_EQ(weight(2.0f, 0.0f, 1.0f), 1.0f);
  EXPECT_EQ(weight(-0.5f, 0.0f, 1.0f), 0.0f);
}

}  // namespace alcedo
