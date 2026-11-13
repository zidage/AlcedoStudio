//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tone Curve presets (#891, tone_curve_preset_study.md): the point rules, 18% grey, monotony, and
// the neutral axis that the render gives with each preset against the source tone of the study.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <string>
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

auto             Code(double display) -> double {
  return 255.0 * std::pow(std::clamp(display, 0.0, 1.0), 1.0 / 2.2);
}

auto Solve(const ToneCurvePreset& preset) -> tone_curve::ToneCurveSpline {
  return tone_curve::SolveToneCurveSpline(preset.points);
}

auto AxisSample(int index) -> float {
  return static_cast<float>(index) / static_cast<float>(tone_curve_study::kAxisSampleCount - 1);
}

TEST(ToneCurvePresetTest, MenuListsTheNinePresetsWithUniqueIds) {
  const auto presets = ToneCurvePresets();
  ASSERT_EQ(presets.size(), 9U);
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
  EXPECT_EQ(FindToneCurvePreset(kFilmLikeToneCurvePresetId)->display_name, "Film-like");
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
  ASSERT_EQ(tone_curve_study::kPresetStudies.size(), ToneCurvePresets().size() - 1);
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
