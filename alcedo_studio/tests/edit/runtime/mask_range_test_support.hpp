//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "ai_mask_raster_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "mask_range_reference.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"

/**
 * @file mask_range_test_support.hpp
 * @brief Luminance Range and Hue Range checks shared by the CUDA, OpenCL, and Metal mask tests.
 *
 * Each backend fixture fills a @ref MaskRangeBackendHarness at the @ref
 * mask_range_reference::kSceneExtent render extent and calls one Check function per test.
 */
namespace alcedo::mask_range_test {

using mask_range_reference::ScenePixel;

/// Backend operations of the checks. The fixture document renders at kSceneExtent.
struct MaskRangeBackendHarness {
  /// Backend name for paths under build/tmp.
  std::string                           backend;
  std::function<PipelineDocument&()>    document;
  /// Compile the fixture document with the default render request.
  std::function<void()>                 compile;
  std::function<const ExecutionPlan&()> plan;
  /**
   * MaskEvaluate of Mask @p mask_id of the first Grade, with @p scene as the Grade input: an
   * RGBA32F cached image that the fixture uploads. An empty @p scene binds a cached image id that
   * the workspace does not hold. Returns the effective R8; throws the evaluate error.
   */
  std::function<std::vector<std::uint8_t>(const MaskId&, std::span<const ScenePixel>)>
                                                                   render_mask_with_scene;
  std::function<void(std::shared_ptr<const MaskAssetReader>)>      set_reader;
  /// Execute the whole compiled plan through the plan executor, then WaitIdle.
  std::function<void()>                                            execute_plan;
  std::function<std::vector<ScenePixel>(const FrameSceneBinding&)> download_scene;
  std::function<std::vector<std::uint8_t>(const GraphValueId&)>    download_r8;
};

/// Linear Gradient whose source coverage is 1 at every pixel, so the coverage is the range weight.
inline auto MakeFullCoverageMask(std::string id) -> MaskModel {
  LinearGradientMaskSource source;
  source.start_value = 1.0f;
  source.end_value   = 1.0f;
  return grade_mask_test::MakeLinearGradientMask(MaskId{std::move(id)}, source);
}

/// Radial with a soft edge, so the source coverage has many values.
inline auto MakeSoftRadialMask(std::string id) -> MaskModel {
  RadialMaskSource source;
  source.center_x      = 0.45f;
  source.center_y      = 0.55f;
  source.major_radius  = 0.4f;
  source.minor_radius  = 0.3f;
  source.rotation      = 0.3f;
  source.inner_feather = 0.6f;
  source.outer_feather = 0.4f;
  return grade_mask_test::MakeRadialMask(MaskId{std::move(id)}, source);
}

inline void AddPrimaryMask(MaskRangeBackendHarness& h, MaskModel mask) {
  grade_mask_test::AddMask(*h.document().PrimaryGrade(), std::move(mask));
  h.document().MarkTopologyChanged();
}

/// Number of codes in @p r8 that are 0, 255, and strictly between.
struct CodeSpread {
  std::size_t zero    = 0;
  std::size_t full    = 0;
  std::size_t partial = 0;
};

inline auto SpreadOf(std::span<const std::uint8_t> r8) -> CodeSpread {
  CodeSpread spread;
  for (const auto code : r8) {
    if (code == 0) {
      ++spread.zero;
    } else if (code == 255) {
      ++spread.full;
    } else {
      ++spread.partial;
    }
  }
  return spread;
}

/// Luminance only, Hue only, and both ranges on an inverted Radial with opacity match the double
/// reference within one code; each case has unselected, selected, and transition pixels.
inline void CheckRangesMatchDoublePrecisionReference(MaskRangeBackendHarness& h) {
  auto luminance            = MakeFullCoverageMask("mask.luminance");
  luminance.luminance_range = LuminanceRangeModel{0.3f, 0.7f, 0.05f};
  auto hue                  = MakeFullCoverageMask("mask.hue");
  hue.color_range           = ColorRangeModel{20.0f, 120.0f, 12.0f};
  auto both                 = MakeSoftRadialMask("mask.both");
  both.invert               = true;
  both.opacity              = 0.8f;
  both.luminance_range      = LuminanceRangeModel{0.2f, 0.9f, 0.1f};
  both.color_range          = ColorRangeModel{200.0f, 330.0f, 20.0f};
  const std::vector<MaskModel> masks{luminance, hue, both};
  for (const auto& mask : masks) AddPrimaryMask(h, mask);
  h.compile();

  const auto scene = mask_range_reference::MakeRampAndHueWheelScene();
  for (const auto& mask : masks) {
    const auto label = std::string{mask.id.Value()};
    const auto expected =
        mask_range_reference::ExpectedAnalyticRangedR8(mask, h.plan().geometry, scene);
    const auto actual = h.render_mask_with_scene(mask.id, scene);
    ai_mask_test::ExpectR8Within1(actual, expected, label);
    const auto spread = SpreadOf(expected);
    EXPECT_GE(spread.zero, 20u) << label;
    EXPECT_GE(spread.partial, 20u) << label;
    if (!mask.invert) {
      EXPECT_GE(spread.full, 20u) << label;
    }
  }
}

/// A Hue Range from 300 to 40 degrees selects hues on both sides of 0 and not the opposite hues.
inline void CheckWrappingHueRangeSelectsHuesOnBothSidesOfZero(MaskRangeBackendHarness& h) {
  auto mask        = MakeFullCoverageMask("mask.wrap");
  mask.color_range = ColorRangeModel{300.0f, 40.0f, 12.0f};
  AddPrimaryMask(h, mask);
  h.compile();
  const auto scene  = mask_range_reference::MakeHueWheelScene();
  const auto actual = h.render_mask_with_scene(mask.id, scene);
  const auto expected =
      mask_range_reference::ExpectedAnalyticRangedR8(mask, h.plan().geometry, scene);
  ai_mask_test::ExpectR8Within1(actual, expected, "wrapping hue range");

  std::size_t below_zero = 0;
  std::size_t above_zero = 0;
  std::size_t opposite   = 0;
  for (std::size_t i = 0; i < scene.size(); ++i) {
    const auto lab = mask_range_reference::SceneOkLab(scene[i]);
    if (mask_range_reference::Chroma(lab) < 0.06) continue;
    const double hue = mask_range_reference::HueDegrees(lab);
    if (hue >= 305.0 && hue < 360.0) {
      EXPECT_GE(actual[i], 254) << "hue " << hue;
      ++below_zero;
    } else if (hue <= 35.0) {
      EXPECT_GE(actual[i], 254) << "hue " << hue;
      ++above_zero;
    } else if (hue >= 100.0 && hue <= 240.0) {
      EXPECT_LE(actual[i], 1) << "hue " << hue;
      ++opposite;
    }
  }
  EXPECT_GE(below_zero, 20u);
  EXPECT_GE(above_zero, 20u);
  EXPECT_GE(opposite, 40u);
}

/// A Luminance Range whose weight is exactly 1 on every scene pixel writes the same bytes as the
/// same Mask without ranges, which the unchanged kernel writes without reading the scene.
inline void CheckFullySelectingRangeMatchesTheMaskWithoutRangesByteForByte(
    MaskRangeBackendHarness& h) {
  auto plain             = MakeSoftRadialMask("mask.plain");
  plain.invert           = true;
  plain.opacity          = 0.8f;
  auto ranged            = plain;
  ranged.id              = MaskId{"mask.ranged"};
  // Every pixel of the hue wheel has OkLab L in [0.45, 0.85]: inside [0, 0.9] with no falloff.
  ranged.luminance_range = LuminanceRangeModel{0.0f, 0.9f, 0.05f};
  AddPrimaryMask(h, plain);
  AddPrimaryMask(h, ranged);
  h.compile();
  const auto scene      = mask_range_reference::MakeHueWheelScene();
  const auto without    = h.render_mask_with_scene(plain.id, {});
  const auto with_range = h.render_mask_with_scene(ranged.id, scene);
  EXPECT_EQ(with_range, without);
  EXPECT_GE(SpreadOf(without).partial, 100u);
}

/**
 * The default saturation threshold keeps the fixed chroma gate of the kernels before the threshold
 * was stored: smoothstep(0.015, 0.045, C), written here with literal edges and not through
 * min_chroma. The hue wheel crosses that gate down its rows.
 */
inline void CheckDefaultMinChromaKeepsTheFixedChromaGate(MaskRangeBackendHarness& h) {
  auto mask        = MakeFullCoverageMask("mask.hue");
  mask.color_range = ColorRangeModel{20.0f, 200.0f, 12.0f};
  ASSERT_EQ(mask.color_range->min_chroma, kColorRangeDefaultMinChroma);
  AddPrimaryMask(h, mask);
  h.compile();
  const auto scene  = mask_range_reference::MakeHueWheelScene();
  const auto actual = h.render_mask_with_scene(mask.id, scene);
  ASSERT_EQ(actual.size(), scene.size());
  const auto& range = *mask.color_range;
  std::size_t gate  = 0;
  for (std::size_t i = 0; i < scene.size(); ++i) {
    const auto   lab    = mask_range_reference::SceneOkLab(scene[i]);
    const double hue    = mask_range_reference::HueDegrees(lab);
    double       offset = hue - range.hue_start;
    if (offset < 0.0) offset += 360.0;
    const double width    = range.hue_end - range.hue_start;
    const double distance = offset <= width ? 0.0 : std::min(offset - width, 360.0 - offset);
    const double chroma   = mask_range_reference::Chroma(lab);
    const double weight = (1.0 - mask_range_reference::SmoothStep(0.0, range.softness, distance)) *
                          mask_range_reference::SmoothStep(0.015, 0.045, chroma);
    const auto expected = mask_range_reference::RangedCoverageR8(1.0, mask, weight);
    EXPECT_NEAR(actual[i], expected, 1) << "index " << i << " chroma " << chroma;
    if (distance == 0.0 && chroma > 0.02 && chroma < 0.04) ++gate;
  }
  EXPECT_GE(gate, 20u) << "the scene must cross the chroma gate inside the range";
}

/**
 * A raised saturation threshold drops the pixels that the default threshold selects below it:
 * the output matches the reference with the stored min_chroma, pixels well below the threshold
 * are 0, and pixels well above it are selected as with the default threshold.
 */
inline void CheckRaisedMinChromaDropsLowChromaPixels(MaskRangeBackendHarness& h) {
  auto standard        = MakeFullCoverageMask("mask.standard");
  standard.color_range = ColorRangeModel{20.0f, 200.0f, 12.0f};
  auto raised          = MakeFullCoverageMask("mask.raised");
  raised.color_range   = ColorRangeModel{20.0f, 200.0f, 12.0f, 0.09f};
  AddPrimaryMask(h, standard);
  AddPrimaryMask(h, raised);
  h.compile();
  const auto scene         = mask_range_reference::MakeHueWheelScene();
  const auto with_standard = h.render_mask_with_scene(standard.id, scene);
  const auto with_raised   = h.render_mask_with_scene(raised.id, scene);
  ai_mask_test::ExpectR8Within1(
      with_raised, mask_range_reference::ExpectedAnalyticRangedR8(raised, h.plan().geometry, scene),
      "raised saturation threshold");

  std::size_t dropped = 0;
  std::size_t kept    = 0;
  for (std::size_t i = 0; i < scene.size(); ++i) {
    const auto   lab    = mask_range_reference::SceneOkLab(scene[i]);
    const double chroma = mask_range_reference::Chroma(lab);
    const double hue    = mask_range_reference::HueDegrees(lab);
    if (hue < 40.0 || hue > 180.0) continue;
    if (chroma > 0.05 && chroma < 0.07) {
      EXPECT_GE(with_standard[i], 254) << "index " << i;
      EXPECT_EQ(with_raised[i], 0) << "index " << i;
      ++dropped;
    } else if (chroma > 0.11) {
      EXPECT_GE(with_raised[i], 254) << "index " << i;
      EXPECT_NEAR(with_raised[i], with_standard[i], 1) << "index " << i;
      ++kept;
    }
  }
  EXPECT_GE(dropped, 20u);
  EXPECT_GE(kept, 20u);
}

/// A Mask without ranges does not read the Grade input; a Mask with a range fails without it.
inline void CheckOnlyRangedMasksReadTheGradeInput(MaskRangeBackendHarness& h) {
  const auto plain = MakeSoftRadialMask("mask.plain");
  auto       hue   = MakeFullCoverageMask("mask.hue");
  hue.color_range  = ColorRangeModel{20.0f, 120.0f, 12.0f};
  AddPrimaryMask(h, plain);
  AddPrimaryMask(h, hue);
  h.compile();
  const std::vector<MaskModel> plain_only{plain};
  ai_mask_test::ExpectR8Within1(
      h.render_mask_with_scene(plain.id, {}),
      multi_mask_test::EvaluateEnabledUnionR8(plain_only, h.plan().geometry),
      "Mask without ranges and without a Grade input");
  EXPECT_THROW((void)h.render_mask_with_scene(hue.id, {}), std::runtime_error);
}

/// An AI Mask with a Hue Range and a Luminance Range is its raster coverage times the weights.
inline void CheckAiMaskRangeMatchesReference(MaskRangeBackendHarness& h) {
  const auto asset =
      ai_mask_test::MakeEllipseAsset(ai_mask_test::kAnalysisExtent, RectI{4, 4, 56, 40});
  auto reader = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  h.set_reader(reader);
  auto mask            = ai_mask_test::MakeAiMask(asset, ai_mask_test::kAnalysisExtent,
                                                  std::string{ai_mask_test::kAiMaskId.Value()});
  mask.color_range     = ColorRangeModel{150.0f, 300.0f, 15.0f};
  mask.luminance_range = LuminanceRangeModel{0.2f, 0.8f, 0.05f};
  AddPrimaryMask(h, mask);
  h.compile();
  const auto scene  = mask_range_reference::MakeRampAndHueWheelScene();
  const auto source = ai_mask_test::ExpectedAiMaskR8(asset, {}, h.plan(), false, 1.0);
  std::vector<std::uint8_t> expected(source.size());
  for (std::size_t i = 0; i < source.size(); ++i) {
    expected[i] = mask_range_reference::RangedCoverageR8(
        source[i] / 255.0, mask, mask_range_reference::RangeWeight(mask, scene[i]));
  }
  const auto actual = h.render_mask_with_scene(mask.id, scene);
  ai_mask_test::ExpectR8Within1(actual, expected, "AI Mask with ranges");
  const auto spread = SpreadOf(expected);
  EXPECT_GE(spread.full, 20u);
  EXPECT_GE(spread.partial, 20u);
}

/**
 * Grade 1 sets every pixel to one orange color (a constant 3D LUT). A Hue Range on a Grade 2
 * Mask around that hue selects every pixel: the plan executor evaluates it on the Grade 1 output,
 * which is a scene-work image, and not on the Develop image that enters Grade 1.
 *
 * The Grade 1 output is read from a render of Grade 1 alone. In the two-Grade render the
 * Diffusion Filter writes its result into the free scene-work member, which is the Grade 1 output.
 */
inline void CheckSecondGradeHueRangeReadsFirstGradeOutput(MaskRangeBackendHarness& h) {
  auto& document  = h.document();
  document        = multi_grade_test::MakeIdentityGradeDocument();
  const auto cube = std::filesystem::absolute("build/tmp/mask_range/" + h.backend + "/orange.cube");
  multi_grade_test::WriteConstantRgbCube(cube, 0.62f, 0.38f, 0.12f);
  multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.primary"}, type_ids::Lmt())
      .SetCubePath(cube.string());
  h.compile();
  h.execute_plan();
  const auto first_output =
      h.download_scene(FrameSceneBinding::WorkImage(SceneWorkMember::Member0));
  const auto develop =
      h.download_scene(FrameSceneBinding::CachedImage(h.plan().grade_nodes.front().scene_input));
  ASSERT_FALSE(first_output.empty());
  const auto orange = mask_range_reference::SceneOkLab(first_output.front());
  ASSERT_GT(mask_range_reference::Chroma(orange), 0.06);
  const double hue = mask_range_reference::HueDegrees(orange);

  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  auto* second = multi_grade_test::GradeNode(document, "grade.b");
  ASSERT_NE(second, nullptr);
  auto mask        = MakeFullCoverageMask("mask.hue");
  mask.color_range = ColorRangeModel{static_cast<float>(std::fmod(hue + 340.0, 360.0)),
                                     static_cast<float>(std::fmod(hue + 20.0, 360.0)), 12.0f};
  grade_mask_test::AddMask(*second, mask);
  document.MarkTopologyChanged();
  h.compile();
  h.execute_plan();
  ASSERT_EQ(h.plan().grade_nodes.size(), 2u);
  const auto& second_grade = h.plan().grade_nodes[1];
  ASSERT_TRUE(second_grade.mask_stack.has_value());
  const auto actual = h.download_r8(second_grade.mask_stack->sources.front().effective_output);
  ASSERT_EQ(actual.size(), first_output.size());
  ASSERT_EQ(actual.size(), develop.size());

  std::size_t follows_develop = 0;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const auto from_first = mask_range_reference::RangedCoverageR8(
        1.0, mask, mask_range_reference::RangeWeight(mask, first_output[i]));
    const auto from_develop = mask_range_reference::RangedCoverageR8(
        1.0, mask, mask_range_reference::RangeWeight(mask, develop[i]));
    EXPECT_NEAR(actual[i], from_first, 1) << "index " << i;
    if (std::abs(static_cast<int>(actual[i]) - static_cast<int>(from_develop)) <= 1) {
      ++follows_develop;
    }
  }
  EXPECT_EQ(actual.front(), 255);
  EXPECT_LT(follows_develop, actual.size() / 2) << "the range read the Grade 1 input";
}

}  // namespace alcedo::mask_range_test
