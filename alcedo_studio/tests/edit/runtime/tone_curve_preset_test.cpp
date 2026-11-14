//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tone Curve presets (#891, tone_curve_preset_study.md): the point rules, 18% grey, monotony, the
// neutral axis that the render gives with each fitted preset against the source tone of the
// study, and the S shape of Default (Film-like), placed by hand (#914).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"
#include "edit/runtime/profile_tone_reference.hpp"
#include "tone_curve_preset_study_data.hpp"

namespace alcedo {
namespace {

/// 18% grey on the curve axis, the fixed point of each preset.
constexpr float  kAxisGrey             = CE_ACESCC_MIDDLE_GREY;
/// Largest distance of a preset from 18% grey at 18% grey: 0.002 stop on the curve axis.
constexpr float  kGreyTolerance        = 1e-4f;
/// The smallest x step between two points of the curve panel (curve::kCurveMinPointSpacing).
constexpr float  kPanelPointSpacing    = 0.001f;
/// Allowance of the rendered axis over the recorded fit error: float evaluation and the product
/// neutral response against the tone scale of the study.
constexpr double kRenderAllowanceCodes = 0.05;
/// Largest number of points of Default (Film-like), as the auto-matched curve of RawTherapee.
constexpr std::size_t kFilmLikeMaxPoints    = 8;
/// Curve output at which ACES 2.0 reaches the display peak: the end point of Default (Film-like).
constexpr float       kAxisAtPeak           = 0.95434f;
/// Slope steps of the S shape check on the curve axis.
constexpr int         kSlopeSteps           = 1024;
/// How far the slope may go against the S: the end segments take their end slope from the
/// segment itself, so their middle dips about 0.05 below it (study, section 4).
constexpr double      kSlopeAgainstS        = 0.06;

auto             Code(double display) -> double {
  return 255.0 * std::pow(std::clamp(display, 0.0, 1.0), 1.0 / 2.2);
}

auto Solve(const ToneCurvePreset& preset) -> tone_curve::ToneCurveSpline {
  return tone_curve::SolveToneCurveSpline(preset.points);
}

auto AxisSample(int index) -> float {
  return static_cast<float>(index) / static_cast<float>(tone_curve_study::kAxisSampleCount - 1);
}

TEST(ToneCurvePresetTest, MenuListsTheEightPresetsWithUniqueIds) {
  const auto presets = ToneCurvePresets();
  ASSERT_EQ(presets.size(), 8U);
  std::set<std::string> ids;
  for (const auto& preset : presets) {
    EXPECT_TRUE(ids.insert(std::string{preset.id}).second) << preset.id;
    EXPECT_FALSE(preset.display_name.empty()) << preset.id;
    EXPECT_EQ(FindToneCurvePreset(preset.id), &preset);
  }
  EXPECT_EQ(presets.front().id, "linear");
  EXPECT_EQ(presets.front().PointList(), IdentityCurvePoints());
  EXPECT_EQ(FindToneCurvePreset("no_such_preset"), nullptr);
  ASSERT_NE(FindToneCurvePreset(kFilmLikeToneCurvePresetId), nullptr);
  EXPECT_EQ(FindToneCurvePreset(kFilmLikeToneCurvePresetId)->display_name, "Default (Film-like)");
  // #914: Filmic gave nearly the same curve as Default (Film-like) and left the menu.
  EXPECT_EQ(FindToneCurvePreset("filmic"), nullptr);
}

TEST(ToneCurvePresetTest, EachPresetHasTwoToSixteenPointsWithRisingXInTheUnitSquare) {
  for (const auto& preset : ToneCurvePresets()) {
    SCOPED_TRACE(std::string{preset.id});
    ASSERT_GE(preset.points.size(), tone_curve::kMinControlPoints);
    ASSERT_LE(preset.points.size(), tone_curve::kMaxControlPoints);
    EXPECT_FALSE(tone_curve::FindToneCurvePointsError(preset.points).has_value());
    for (std::size_t i = 0; i < preset.points.size(); ++i) {
      const auto& point = preset.points[i];
      EXPECT_GE(point.x, 0.0f);
      EXPECT_LE(point.x, 1.0f);
      EXPECT_GE(point.y, 0.0f);
      EXPECT_LE(point.y, 1.0f);
      if (i > 0) {
        // The panel keeps the points as they are: its own spacing rule holds.
        EXPECT_GT(point.x - preset.points[i - 1].x, kPanelPointSpacing) << "point " << i;
      }
    }
  }
}

TEST(ToneCurvePresetTest, EachPresetKeepsGreyAndIsMonotone) {
  for (const auto& preset : ToneCurvePresets()) {
    SCOPED_TRACE(std::string{preset.id});
    const auto spline = Solve(preset);
    EXPECT_NEAR(tone_curve::EvaluateToneCurveSpline(spline, kAxisGrey), kAxisGrey, kGreyTolerance);
    float previous = tone_curve::EvaluateToneCurveSpline(spline, -0.1f);
    for (int i = -400; i <= 4400; ++i) {
      const float x     = static_cast<float>(i) / 4000.0f;
      const float value = tone_curve::EvaluateToneCurveSpline(spline, x);
      ASSERT_GE(value, previous) << "x " << x;
      previous = value;
    }
  }
}

TEST(ToneCurvePresetTest, MatchFindsThePresetOfExactlyItsPoints) {
  for (const auto& preset : ToneCurvePresets()) {
    const auto points = preset.PointList();
    EXPECT_EQ(MatchToneCurvePreset(points), &preset) << preset.id;
    auto moved = points;
    moved[moved.size() / 2].y += 1e-4f;
    EXPECT_EQ(MatchToneCurvePreset(moved), nullptr) << preset.id;
  }
}

TEST(ToneCurvePresetTest, DefaultFilmLikeIsOneSCurveWithTheShadowsBelowTheIdentity) {
  // #914: a few points placed by hand on the EV axis. From (0, 0) the slope rises to one
  // inflection above 18% grey (a convex toe: the shadows stay at or below the identity line) and
  // falls from there to the display peak at x = 1 (a concave shoulder).
  const auto* preset = FindToneCurvePreset(kFilmLikeToneCurvePresetId);
  ASSERT_NE(preset, nullptr);
  EXPECT_LE(preset->points.size(), kFilmLikeMaxPoints);
  EXPECT_EQ(preset->points.front(), (CurvePoint{0.0f, 0.0f}));
  EXPECT_EQ(preset->points.back(), (CurvePoint{1.0f, kAxisAtPeak}));

  const auto          spline = Solve(*preset);
  std::vector<double> slopes;
  float               previous = tone_curve::EvaluateToneCurveSpline(spline, 0.0f);
  for (int i = 1; i <= kSlopeSteps; ++i) {
    const float x     = static_cast<float>(i) / kSlopeSteps;
    const float value = tone_curve::EvaluateToneCurveSpline(spline, x);
    if (x < kAxisGrey) {
      EXPECT_LE(value, x) << "x " << x;
    }
    slopes.push_back(static_cast<double>(value - previous) * kSlopeSteps);
    previous = value;
  }
  const auto   steepest        = std::ranges::max_element(slopes);
  const auto   inflection      = std::distance(slopes.begin(), steepest);
  const double inflection_x    = static_cast<double>(inflection) / kSlopeSteps;
  double       steepest_so_far = slopes.front();
  double       fall_below      = 0.0;
  for (auto it = slopes.begin(); it != steepest; ++it) {
    steepest_so_far = std::max(steepest_so_far, *it);
    fall_below      = std::max(fall_below, steepest_so_far - *it);
  }
  double flattest_so_far = *steepest;
  double rise_above      = 0.0;
  for (auto it = steepest; it != slopes.end(); ++it) {
    flattest_so_far = std::min(flattest_so_far, *it);
    rise_above      = std::max(rise_above, *it - flattest_so_far);
  }
  std::cout << "film_like: inflection at x " << inflection_x << ", slope " << *steepest
            << "; slope falls " << fall_below << " below it, rises " << rise_above << " above it\n";
  EXPECT_GT(inflection_x, kAxisGrey);
  EXPECT_GT(slopes[static_cast<std::size_t>(kAxisGrey * kSlopeSteps)], 1.0);
  EXPECT_LT(fall_below, kSlopeAgainstS);
  EXPECT_LT(rise_above, kSlopeAgainstS);
}

TEST(ToneCurvePresetTest, FilmLikeCurveIsRecognizedNowAndInEachEarlierFit) {
  const auto* film_like = FindToneCurvePreset(kFilmLikeToneCurvePresetId);
  ASSERT_NE(film_like, nullptr);
  EXPECT_TRUE(IsFilmLikeToneCurve(film_like->points));
  for (const auto earlier : FilmLikeToneCurveEarlierPoints()) {
    EXPECT_TRUE(IsFilmLikeToneCurve(earlier));
    EXPECT_FALSE(std::ranges::equal(earlier, film_like->points));
    // The menu shows the earlier points as a custom curve.
    EXPECT_EQ(MatchToneCurvePreset(earlier), nullptr);
    std::vector<CurvePoint> moved{earlier.begin(), earlier.end()};
    moved.back().y -= 1e-4f;
    EXPECT_FALSE(IsFilmLikeToneCurve(moved));
  }
  EXPECT_FALSE(IsFilmLikeToneCurve(FindToneCurvePreset("medium_contrast")->points));
  EXPECT_FALSE(IsFilmLikeToneCurve(IdentityCurvePoints()));
}

TEST(ToneCurvePresetTest, StudyToneScaleIsTheProductAces2NeutralResponse) {
  // The study fits against the ACES 2.0 SDR tone scale; the render uses the neutral response of
  // the ACES 2.0 output transform.
  const auto response = ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20);
  double     largest  = 0.0;
  for (int i = 0; i < tone_curve_study::kAxisSampleCount; ++i) {
    const double product = response->Forward(CeAcesccDecode(AxisSample(i)));
    largest              = std::max(largest,
                                    std::abs(Code(product) - Code(tone_curve_study::kStudyNeutralResponse[i])));
  }
  std::cout << "largest code difference of the study tone scale " << largest << "\n";
  EXPECT_LT(largest, kRenderAllowanceCodes);
}

TEST(ToneCurvePresetTest, RenderedNeutralAxisAgreesWithTheStudySourceToneWithinTheFitError) {
  // The grade stage applies the curve on the ACEScc axis and ACES 2.0 renders the result: the
  // display light of a neutral at each axis sample, against the source tone of the study.
  const auto response = ProfileToneNeutralResponse::Resolve(DrtMethod::Aces20);
  // Linear and Default (Film-like) have no fit.
  ASSERT_EQ(tone_curve_study::kPresetStudies.size(), ToneCurvePresets().size() - 2);
  for (const auto& study : tone_curve_study::kPresetStudies) {
    SCOPED_TRACE(std::string{study.preset_id});
    const auto* preset = FindToneCurvePreset(study.preset_id);
    ASSERT_NE(preset, nullptr);
    const auto spline  = Solve(*preset);
    double     largest = 0.0;
    for (int i = 0; i < tone_curve_study::kAxisSampleCount; ++i) {
      const float  output   = tone_curve::EvaluateToneCurveSpline(spline, AxisSample(i));
      const double rendered = response->Forward(std::max(CeAcesccDecode(output), 0.0f));
      largest = std::max(largest, std::abs(Code(rendered) - Code(study.source_display[i])));
    }
    std::cout << study.preset_id << ": largest code difference " << largest << ", fit error "
              << study.fit_error_codes << "\n";
    EXPECT_LE(largest, study.fit_error_codes + kRenderAllowanceCodes);
  }
}

}  // namespace
}  // namespace alcedo
