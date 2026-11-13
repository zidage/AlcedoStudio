//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "oklab_contrast_reference.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace alcedo {
namespace {

namespace ref = oklab_contrast_reference;

auto Chroma(const ref::Rgb& lab) -> float { return std::hypot(lab[1], lab[2]); }
auto Hue(const ref::Rgb& lab) -> float { return std::atan2(lab[2], lab[1]); }
auto Luminance(const ref::Rgb& ap1) -> float {
  return 0.2722287168f * ap1[0] + 0.6740817658f * ap1[1] + 0.0536895174f * ap1[2];
}
auto StopsFromGrey(float luminance) -> float { return std::log2(luminance / 0.18f); }

TEST(OkLabContrastReference, WhiteNormalizedMatrixMapsAp1NeutralsToZeroChroma) {
  for (const float y : {0.01f, 0.18f, 1.0f, 8.0f}) {
    const auto lab = ref::LinearAp1ToOkLab({y, y, y});
    EXPECT_NEAR(lab[0], std::cbrt(y), 1.0e-5f * std::max(1.0f, lab[0]));
    EXPECT_NEAR(lab[1], 0.0f, 1.0e-6f);
    EXPECT_NEAR(lab[2], 0.0f, 1.0e-6f);
  }
}

TEST(OkLabContrastReference, OkLabRoundTripReturnsLinearAp1) {
  const ref::Rgb color{0.42f, 0.11f, 0.03f};
  const auto     back = ref::OkLabToLinearAp1(ref::LinearAp1ToOkLab(color));
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(back[i], color[i], 1.0e-5f);
}

TEST(OkLabContrastReference, ZeroContrastIsExactIdentity) {
  const ref::Rgb color{0.42f, 0.11f, 0.03f};
  EXPECT_EQ(ref::ApplyContrastLinearAp1(color, 0.0f), color);
}

TEST(OkLabContrastReference, EighteenPercentGreyIsUnchangedAtEveryContrast) {
  for (const float contrast : {-100.0f, -40.0f, 15.0f, 60.0f, 100.0f}) {
    const auto out = ref::ApplyContrastLinearAp1({0.18f, 0.18f, 0.18f}, contrast);
    for (const float channel : out) EXPECT_NEAR(channel, 0.18f, 1.0e-5f) << contrast;
  }
}

TEST(OkLabContrastReference, NeutralsStayNeutralAndMoveAwayFromGreyForPositiveContrast) {
  for (const float y : {0.005f, 0.05f, 0.7f, 4.0f}) {
    const auto out = ref::ApplyContrastLinearAp1({y, y, y}, 50.0f);
    EXPECT_NEAR(out[0], out[1], 1.0e-5f * std::max(1.0f, out[1]));
    EXPECT_NEAR(out[2], out[1], 1.0e-5f * std::max(1.0f, out[1]));
    if (y < 0.18f) {
      EXPECT_LT(out[1], y);
    } else {
      EXPECT_GT(out[1], y);
    }
  }
}

TEST(OkLabContrastReference, MidGreySlopeInStopsEqualsTwoToTheContrastOverHundred) {
  const float step = 0.01f;
  for (const float contrast : {-100.0f, 15.0f, 100.0f}) {
    const float lo    = 0.18f * std::exp2(-step);
    const float hi    = 0.18f * std::exp2(step);
    const float y_lo  = ref::ApplyContrastLinearAp1({lo, lo, lo}, contrast)[1];
    const float y_hi  = ref::ApplyContrastLinearAp1({hi, hi, hi}, contrast)[1];
    const float slope = (StopsFromGrey(y_hi) - StopsFromGrey(y_lo)) / (2.0f * step);
    EXPECT_NEAR(slope, std::exp2(contrast * 0.01f), 2.0e-3f) << contrast;
  }
}

TEST(OkLabContrastReference, ShiftFarFromGreyIsBoundedByCurveWidth) {
  const float slope = std::exp2(1.0f);
  const float bound = (slope - 1.0f) * ref::kCurveWidthStops;
  for (const float stops : {-12.0f, -6.0f, 6.0f, 12.0f}) {
    const float y     = 0.18f * std::exp2(stops);
    const float out   = ref::ApplyContrastLinearAp1({y, y, y}, 100.0f)[1];
    const float shift = StopsFromGrey(out) - stops;
    EXPECT_LE(std::fabs(shift), bound + 1.0e-3f) << stops;
    EXPECT_GT(std::fabs(shift), 0.9f * bound) << stops;
  }
}

TEST(OkLabContrastReference, LightnessResponseIsMonotonicForNegativeAndPositiveContrast) {
  for (const float contrast : {-100.0f, 100.0f}) {
    float previous = -1.0f;
    for (float stops = -14.0f; stops <= 8.0f; stops += 0.125f) {
      const float y   = 0.18f * std::exp2(stops);
      const float out = ref::ApplyContrastLinearAp1({y, y, y}, contrast)[1];
      EXPECT_GT(out, previous) << contrast << " @ " << stops;
      previous = out;
    }
  }
}

TEST(OkLabContrastReference, HueIsPreservedAndBrightenedChromaScalesBySquareRootOfSlope) {
  // Bright orange (about +1.9 stops): positive contrast brightens it, so chroma scales by sqrt(k).
  const ref::Rgb color{2.4f, 0.6f, 0.15f};
  const auto     before = ref::LinearAp1ToOkLab(color);
  for (const float contrast : {15.0f, 80.0f}) {
    const auto after = ref::LinearAp1ToOkLab(ref::ApplyContrastLinearAp1(color, contrast));
    EXPECT_GT(after[0], before[0]) << contrast;
    EXPECT_NEAR(Hue(after), Hue(before), 1.0e-4f) << contrast;
    EXPECT_NEAR(Chroma(after) / Chroma(before), std::sqrt(std::exp2(contrast * 0.01f)), 1.0e-4f)
        << contrast;
  }
}

TEST(OkLabContrastReference, DarkenedColorsRaiseSaturationByOnlySquareRootOfSlope) {
  // Dark red below grey (positive contrast darkens it) and bright orange (negative contrast darkens
  // it): chroma shrinks with lightness, so C / L grows by exactly sqrt(k) and hue is unchanged.
  const std::array<std::pair<ref::Rgb, float>, 2> cases{
      {{{0.02f, 0.004f, 0.002f}, 60.0f}, {{2.4f, 0.6f, 0.15f}, -60.0f}}};
  for (const auto& [color, contrast] : cases) {
    const auto before = ref::LinearAp1ToOkLab(color);
    const auto after  = ref::LinearAp1ToOkLab(ref::ApplyContrastLinearAp1(color, contrast));
    EXPECT_LT(after[0], before[0]) << contrast;
    EXPECT_NEAR(Hue(after), Hue(before), 1.0e-4f) << contrast;
    EXPECT_NEAR((Chroma(after) / after[0]) / (Chroma(before) / before[0]),
                std::sqrt(std::exp2(contrast * 0.01f)), 1.0e-4f)
        << contrast;
  }
}

TEST(OkLabContrastReference, NonPositiveLightnessUsesContinuousBlackGain) {
  const float slope      = std::exp2(0.5f);
  const float black_gain = std::exp2(-(slope - 1.0f) * ref::kCurveWidthStops / 3.0f);
  EXPECT_NEAR(ref::LightnessGain(1.0e-12f, slope), black_gain, 1.0e-4f);
  EXPECT_FLOAT_EQ(ref::LightnessGain(0.0f, slope), black_gain);
  EXPECT_FLOAT_EQ(ref::LightnessGain(-0.2f, slope), black_gain);
}

TEST(OkLabContrastReference, PositiveContrastKeepsAverageLuminanceOfSymmetricStopsNearGrey) {
  // A symmetric scene around grey (+/-2 stops) should not brighten overall: the geometric mean
  // luminance stays at 18%.
  const float lo     = 0.18f * std::exp2(-2.0f);
  const float hi     = 0.18f * std::exp2(2.0f);
  const float out_lo = ref::ApplyContrastLinearAp1({lo, lo, lo}, 40.0f)[1];
  const float out_hi = ref::ApplyContrastLinearAp1({hi, hi, hi}, 40.0f)[1];
  const float geomean =
      std::sqrt(Luminance({out_lo, out_lo, out_lo}) * Luminance({out_hi, out_hi, out_hi}));
  EXPECT_NEAR(geomean, 0.18f, 1.0e-4f);
}

}  // namespace
}  // namespace alcedo
