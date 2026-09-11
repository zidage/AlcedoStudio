//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QPointF>
#include <QRectF>
#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "app/editor_mask_creation_controller.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/analytic_mask_edit.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/grade_mask_coverage.hpp"
#include "edit/mask/mask_list_selection.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"

namespace alcedo {
namespace {

constexpr Extent2D kRaster{64, 32};

[[nodiscard]] auto MakeMapping() -> MaskEditViewMapping {
  MaskEditViewMapping mapping;
  mapping.widget     = {200, 100, 1.0f};
  mapping.photograph = {static_cast<int>(kRaster.width), static_cast<int>(kRaster.height)};
  mapping.zoom       = 1.25f;
  mapping.pan        = QVector2D(4.0f, -3.0f);
  mapping.geometry   = MaskEditGeometry::MakeIdentityPhotographGeometry(kRaster);
  return mapping;
}

[[nodiscard]] auto SampleOf(const MaskEditViewMapping& mapping, QPointF item, bool allow_outside)
    -> MaskCreationSample {
  const auto mapped = MaskEditGeometry::MapItemToReference(mapping, item, allow_outside);
  EXPECT_TRUE(mapped.has_value());
  MaskCreationSample sample;
  sample.normalized        = mapped->normalized;
  sample.reference_pixels  = mapped->reference_pixels;
  sample.inside_photograph = mapped->inside_photograph;
  return sample;
}

[[nodiscard]] auto ItemOf(const MaskEditViewMapping& mapping, Vector2 normalized) -> QPointF {
  const auto item = MapNormalizedMaskPointToItem(mapping, normalized);
  EXPECT_TRUE(item.has_value());
  return item.value_or(QPointF());
}

// Independent native Radial coverage from the NM7 plan equations. Not overlay layout.
[[nodiscard]] auto IndependentRadialCoverage(const RadialMaskSource& source, Vector2 q) -> float {
  const float c = std::cos(source.rotation);
  const float s = std::sin(source.rotation);
  const float u = (c * (q.x - source.center_x) + s * (q.y - source.center_y)) /
                  std::max(source.major_radius, 1.0e-6f);
  const float v = (-s * (q.x - source.center_x) + c * (q.y - source.center_y)) /
                  std::max(source.minor_radius, 1.0e-6f);
  const float rho   = std::sqrt(u * u + v * v);
  const float inner = std::max(0.0f, 1.0f - source.inner_feather);
  const float outer = 1.0f + source.outer_feather;
  return 1.0f - std::clamp((rho - inner) / std::max(outer - inner, 1.0e-6f), 0.0f, 1.0f);
}

[[nodiscard]] auto IndependentLinearCoverage(const LinearGradientMaskSource& source, Vector2 q)
    -> float {
  const float length = std::hypot(source.normal_x, source.normal_y);
  const float nx     = source.normal_x / std::max(length, 1.0e-6f);
  const float ny     = source.normal_y / std::max(length, 1.0e-6f);
  const float d      = (q.x - source.origin_x) * nx + (q.y - source.origin_y) * ny;
  const float t = std::clamp(d / std::max(source.transition_distance, 1.0e-6f) + 0.5f, 0.0f, 1.0f);
  return source.start_value + (source.end_value - source.start_value) * t;
}

[[nodiscard]] auto IndependentR8(float coverage) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
}

[[nodiscard]] auto TexelNormalized(std::uint32_t x, std::uint32_t y) -> Vector2 {
  const auto center = CanonicalBrushTexelReferenceCenter(x, y, kRaster, kRaster);
  return {center.x / static_cast<float>(kRaster.width),
          center.y / static_cast<float>(kRaster.height)};
}

[[nodiscard]] auto MixAt(const GradeMaskCoverage& mix, std::uint32_t x, std::uint32_t y)
    -> std::uint8_t {
  return mix.Pixels()[PackedR8Index(x, y, kRaster)];
}

void ExpectMixMatchesIndependent(const GradeMaskCoverage& mix, const MaskModel& mask) {
  ASSERT_EQ(mix.Pixels().size(), static_cast<std::size_t>(kRaster.width) * kRaster.height);
  std::uint32_t mismatches = 0;
  std::int32_t  max_err    = 0;
  for (std::uint32_t y = 0; y < kRaster.height; y += 4) {
    for (std::uint32_t x = 0; x < kRaster.width; x += 4) {
      const Vector2 q        = TexelNormalized(x, y);
      float         coverage = 0.0f;
      if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
        coverage = IndependentRadialCoverage(*radial, q);
      } else {
        coverage = IndependentLinearCoverage(std::get<LinearGradientMaskSource>(mask.source), q);
      }
      if (mask.invert) {
        coverage = 1.0f - coverage;
      }
      const auto expected = IndependentR8(std::clamp(coverage * mask.opacity, 0.0f, 1.0f));
      const auto err = std::abs(static_cast<int>(MixAt(mix, x, y)) - static_cast<int>(expected));
      max_err        = std::max(max_err, err);
      if (err > 1) {
        ++mismatches;
      }
    }
  }
  EXPECT_EQ(mismatches, 0u) << "max R8 error " << max_err;
}

[[nodiscard]] auto CopyMix(const GradeMaskCoverage& mix) -> std::vector<std::uint8_t> {
  const auto pixels = mix.Pixels();
  return {pixels.begin(), pixels.end()};
}

struct AnalyticCreationHarness {
  AnalyticCreationHarness()
      : graph(std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(17))),
        journal(std::make_shared<MiniGitJournal>()),
        history(graph, journal) {
    document = CreateDefaultPipelineDocument();
    mix.SetGeometry(kRaster, kRaster);
    controller.Bind(document, history);
    controller.SetInteractivePreview([this]() {
      ++preview_count;
      mix.EvaluateFull(document.PrimaryGrade()->Masks());
      last_mix = CopyMix(mix);
    });
  }

  std::shared_ptr<CommitGraph>    graph;
  std::shared_ptr<MiniGitJournal> journal;
  MiniGitWorkingHistory           history;
  PipelineDocument                document;
  EditorMaskCreationController    controller;
  GradeMaskCoverage               mix;
  std::vector<std::uint8_t>       last_mix;
  int                             preview_count = 0;
  MaskEditViewMapping             mapping       = MakeMapping();
  EditorSessionIdentity           session{17, 4};
  MaskPointerIdentity             pointer{3, 1, 9};
};

[[nodiscard]] auto OverlayFillCount(const MaskOverlayDisplay& display) -> int {
  return BuildMaskOverlaySceneGeometry(display, DefaultMaskOverlayStyle())
      .coverage_fill_vertex_count;
}

[[nodiscard]] auto BatchFromCommit(const EditCommit& commit) -> PipelineEditBatch {
  return PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
}

}  // namespace

TEST(AnalyticMaskCreationTest, ExistingRadialMoveUpdatesInteractivePixelsBeforeRelease) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x     = 0.50f;
  radial.center_y     = 0.50f;
  radial.major_radius = 0.22f;
  radial.minor_radius = 0.16f;
  radial.rotation     = 0.40f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  const auto mix_before  = CopyMix(harness.mix);
  const auto head_before = harness.history.working_head();

  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialCenter, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.28f, 0.62f}), true);
  const auto preview = harness.controller.AppendMaskInput(moved, harness.pointer);
  ASSERT_TRUE(preview.accepted);
  EXPECT_TRUE(preview.interactive_preview);
  EXPECT_FALSE(preview.committed);
  EXPECT_FALSE(preview.quality_requested);
  EXPECT_GE(harness.preview_count, 1);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_NE(harness.last_mix, mix_before);

  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_NEAR(live->center_x, 0.28f, 1.0e-2f);
  EXPECT_NEAR(live->center_y, 0.62f, 1.0e-2f);
  EXPECT_FLOAT_EQ(live->major_radius, radial.major_radius);
  EXPECT_FLOAT_EQ(live->minor_radius, radial.minor_radius);
  EXPECT_FLOAT_EQ(live->rotation, radial.rotation);
  ExpectMixMatchesIndependent(harness.mix,
                              *harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"}));

  const auto display =
      MakeRadialExistingOverlayDisplay(harness.mapping, *live, DefaultMaskOverlayStyle(), QRectF());
  EXPECT_EQ(OverlayFillCount(display), 0);
  EXPECT_FALSE(display.handles.empty());

  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_TRUE(finish.quality_requested);
  EXPECT_NE(harness.history.working_head(), head_before);
}

TEST(AnalyticMaskCreationTest, FinishModeSettlesOnceAndLeavesMaskEditingInactive) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x     = 0.5f;
  radial.center_y     = 0.5f;
  radial.major_radius = 0.22f;
  radial.minor_radius = 0.16f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.72f, 0.5f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialMajor, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved = SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.80f, 0.5f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  const auto head_before = harness.history.working_head();

  const auto finish      = harness.controller.FinishCreationMode();
  EXPECT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_TRUE(finish.quality_requested);
  EXPECT_EQ(harness.controller.state(), EditorMaskCreationState::Inactive);
  EXPECT_NE(harness.history.working_head(), head_before);
  const auto settled_head = harness.history.working_head();

  const auto duplicate    = harness.controller.FinishCreationMode();
  EXPECT_TRUE(duplicate.accepted);
  EXPECT_FALSE(duplicate.committed);
  EXPECT_FALSE(duplicate.quality_requested);
  EXPECT_EQ(harness.history.working_head(), settled_head);
}

TEST(AnalyticMaskCreationTest, ExistingGradientMovePreservesDirectionAndUpdatesInteractivePixels) {
  AnalyticCreationHarness  harness;
  LinearGradientMaskSource linear;
  linear.origin_x            = 0.50f;
  linear.origin_y            = 0.50f;
  linear.normal_x            = 1.0f;
  linear.normal_y            = 0.0f;
  linear.transition_distance = 0.40f;
  grade_mask_test::AddLinearGradientMask(harness.document, MaskId{"mask.linear"}, linear);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  const auto mix_before  = CopyMix(harness.mix);
  const auto head_before = harness.history.working_head();

  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.linear"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::LinearOrigin, press, harness.pointer,
                          MaskSourceKind::LinearGradient)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.70f, 0.50f}), true);
  const auto preview = harness.controller.AppendMaskInput(moved, harness.pointer);
  ASSERT_TRUE(preview.accepted);
  EXPECT_TRUE(preview.interactive_preview);
  EXPECT_FALSE(preview.committed);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_NE(harness.last_mix, mix_before);

  const auto* live = std::get_if<LinearGradientMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.linear"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_NEAR(live->origin_x, 0.70f, 1.0e-2f);
  EXPECT_NEAR(live->origin_y, 0.50f, 1.0e-2f);
  EXPECT_FLOAT_EQ(live->normal_x, linear.normal_x);
  EXPECT_FLOAT_EQ(live->normal_y, linear.normal_y);
  EXPECT_FLOAT_EQ(live->transition_distance, linear.transition_distance);
  ExpectMixMatchesIndependent(harness.mix,
                              *harness.document.PrimaryGrade()->FindMask(MaskId{"mask.linear"}));

  const auto display =
      MakeLinearExistingOverlayDisplay(harness.mapping, *live, DefaultMaskOverlayStyle(), QRectF());
  EXPECT_EQ(OverlayFillCount(display), 0);
  EXPECT_FALSE(display.handles.empty());

  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_TRUE(finish.quality_requested);
}

TEST(AnalyticMaskCreationTest, RadialFeatherControlsMatchEvaluator) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x      = 0.50f;
  radial.center_y      = 0.50f;
  radial.major_radius  = 0.24f;
  radial.minor_radius  = 0.18f;
  radial.rotation      = 0.30f;
  radial.inner_feather = 0.10f;
  radial.outer_feather = 0.05f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);

  const Vector2 inner_q{radial.center_x + std::cos(radial.rotation) * radial.major_radius * 0.55f,
                        radial.center_y + std::sin(radial.rotation) * radial.major_radius * 0.55f};
  const auto    press = SampleOf(harness.mapping, ItemOf(harness.mapping, inner_q), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialInnerFeather, press, harness.pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(press, harness.pointer).accepted);

  const Vector2 outer_q{radial.center_x + std::cos(radial.rotation) * radial.major_radius * 1.35f,
                        radial.center_y + std::sin(radial.rotation) * radial.major_radius * 1.35f};
  MaskPointerIdentity outer_pointer = harness.pointer;
  ASSERT_TRUE(harness.controller.FinishMaskInput().accepted);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto outer_press    = SampleOf(harness.mapping, ItemOf(harness.mapping, outer_q), false);
  outer_pointer.sequence_id = 10;
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialOuterFeather, outer_press, outer_pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(outer_press, outer_pointer).accepted);

  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(live->major_radius, radial.major_radius);
  EXPECT_FLOAT_EQ(live->minor_radius, radial.minor_radius);
  EXPECT_NEAR(live->inner_feather, 0.45f, 0.08f);
  EXPECT_NEAR(live->outer_feather, 0.35f, 0.08f);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  ExpectMixMatchesIndependent(harness.mix,
                              *harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"}));

  const auto inner_rho = RadialRho(*live, TexelNormalized(kRaster.width / 2, kRaster.height / 2));
  ASSERT_TRUE(inner_rho.has_value());
  const float expected_center =
      IndependentRadialCoverage(*live, TexelNormalized(kRaster.width / 2, kRaster.height / 2));
  EXPECT_NEAR(CoverageFromMaskR8(MixAt(harness.mix, kRaster.width / 2, kRaster.height / 2)),
              expected_center, 1.0f / 255.0f + 1.0e-6f);
}

TEST(AnalyticMaskCreationTest, DegenerateAnalyticCreationCreatesNoCommit) {
  AnalyticCreationHarness harness;
  const auto              head_before = harness.history.working_head();
  ASSERT_TRUE(harness.controller
                  .BeginCreation(MaskSourceKind::Radial, harness.document.PrimaryGrade()->Id(),
                                 harness.session)
                  .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskInput(press, harness.pointer, MaskSourceKind::Radial).accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(press, harness.pointer).accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
  EXPECT_EQ(harness.preview_count, 0);

  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_FALSE(finish.committed);
  EXPECT_FALSE(finish.quality_requested);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
  EXPECT_EQ(harness.history.working_head(), head_before);

  ASSERT_TRUE(harness.controller
                  .BeginCreation(MaskSourceKind::LinearGradient,
                                 harness.document.PrimaryGrade()->Id(), harness.session)
                  .accepted);
  MaskPointerIdentity linear_pointer = harness.pointer;
  linear_pointer.sequence_id         = 11;
  ASSERT_TRUE(
      harness.controller.BeginMaskInput(press, linear_pointer, MaskSourceKind::LinearGradient)
          .accepted);
  EXPECT_TRUE(harness.controller.FinishMaskInput().accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
  EXPECT_EQ(harness.history.working_head(), head_before);
}

TEST(AnalyticMaskCreationTest, EscapeRestoresAnalyticSourceWithoutCommit) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x     = 0.42f;
  radial.center_y     = 0.58f;
  radial.major_radius = 0.20f;
  radial.minor_radius = 0.14f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  const auto mix_before  = CopyMix(harness.mix);
  const auto head_before = harness.history.working_head();

  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press = SampleOf(
      harness.mapping, ItemOf(harness.mapping, Vector2{radial.center_x, radial.center_y}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialCenter, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.22f, 0.30f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).interactive_preview);
  EXPECT_NE(harness.last_mix, mix_before);

  const auto cancel = harness.controller.CancelMaskInput();
  ASSERT_TRUE(cancel.accepted);
  EXPECT_FALSE(cancel.committed);
  EXPECT_EQ(harness.history.working_head(), head_before);
  const auto* restored = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(restored, nullptr);
  EXPECT_FLOAT_EQ(restored->center_x, radial.center_x);
  EXPECT_FLOAT_EQ(restored->center_y, radial.center_y);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  EXPECT_EQ(CopyMix(harness.mix), mix_before);
}

TEST(AnalyticMaskCreationTest, ValidRadialCreationCommitsOnceAndKeepsControlOnlyOverlay) {
  AnalyticCreationHarness harness;
  const auto              head_before = harness.history.working_head();
  ASSERT_TRUE(harness.controller
                  .BeginCreation(MaskSourceKind::Radial, harness.document.PrimaryGrade()->Id(),
                                 harness.session)
                  .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.45f, 0.40f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskInput(press, harness.pointer, MaskSourceKind::Radial).accepted);
  const auto drag = SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.62f, 0.58f}), true);
  const auto preview = harness.controller.AppendMaskInput(drag, harness.pointer);
  ASSERT_TRUE(preview.accepted);
  EXPECT_TRUE(preview.interactive_preview);
  EXPECT_FALSE(preview.committed);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 1u);
  EXPECT_EQ(harness.history.working_head(), head_before);
  ASSERT_TRUE(harness.controller.OverlayIsCreating());
  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(preview.mask_id)->source);
  ASSERT_NE(live, nullptr);
  const auto display =
      MakeRadialCreatingOverlayDisplay(harness.mapping, *live, DefaultMaskOverlayStyle(), QRectF());
  EXPECT_EQ(OverlayFillCount(display), 0);
  EXPECT_TRUE(display.creation_outline.empty());
  EXPECT_FALSE(display.selected_contours.empty());

  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_TRUE(finish.quality_requested);
  EXPECT_NE(harness.history.working_head(), head_before);
  EXPECT_FALSE(harness.controller.FinishMaskInput().committed);
}

TEST(AnalyticMaskCreationTest, RadialFeatherControlsPreserveCenterRadiiAndRotation) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x      = 0.48f;
  radial.center_y      = 0.52f;
  radial.major_radius  = 0.24f;
  radial.minor_radius  = 0.17f;
  radial.rotation      = 0.55f;
  radial.inner_feather = 0.12f;
  radial.outer_feather = 0.08f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto inner_q = RadialNormalizedPoint(radial, 0.55f, 0.0f);
  const auto press   = SampleOf(harness.mapping, ItemOf(harness.mapping, inner_q), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialInnerFeather, press, harness.pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(press, harness.pointer).accepted);
  ASSERT_TRUE(harness.controller.FinishMaskInput().accepted);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto          outer_q       = RadialNormalizedPoint(radial, 1.40f, 0.0f);
  MaskPointerIdentity outer_pointer = harness.pointer;
  outer_pointer.sequence_id         = 11;
  const auto outer_press = SampleOf(harness.mapping, ItemOf(harness.mapping, outer_q), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialOuterFeather, outer_press, outer_pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(outer_press, outer_pointer).accepted);
  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(live->center_x, radial.center_x);
  EXPECT_FLOAT_EQ(live->center_y, radial.center_y);
  EXPECT_FLOAT_EQ(live->major_radius, radial.major_radius);
  EXPECT_FLOAT_EQ(live->minor_radius, radial.minor_radius);
  EXPECT_NEAR(live->rotation, radial.rotation, 1.0e-5f);
  EXPECT_GT(live->inner_feather, radial.inner_feather);
  EXPECT_GT(live->outer_feather, radial.outer_feather);
}

TEST(AnalyticMaskCreationTest, RadialFeatherDragUpdatesInteractivePixelsBeforeRelease) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x      = 0.50f;
  radial.center_y      = 0.50f;
  radial.major_radius  = 0.22f;
  radial.minor_radius  = 0.16f;
  radial.inner_feather = 0.10f;
  radial.outer_feather = 0.05f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  const auto mix_before  = CopyMix(harness.mix);
  const auto head_before = harness.history.working_head();
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press = SampleOf(
      harness.mapping, ItemOf(harness.mapping, RadialNormalizedPoint(radial, 1.45f, 0.0f)), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialOuterFeather, press, harness.pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  const auto preview = harness.controller.AppendMaskInput(press, harness.pointer);
  ASSERT_TRUE(preview.accepted);
  EXPECT_TRUE(preview.interactive_preview);
  EXPECT_FALSE(preview.committed);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_GE(harness.preview_count, 1);
  EXPECT_NE(harness.last_mix, mix_before);
}

TEST(AnalyticMaskCreationTest, NodeDrawerSelectionLoadsExistingMaskWithoutCreatingOrRendering) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.major_radius = 0.20f;
  radial.minor_radius = 0.16f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  grade_mask_test::AddParameterizedBrushMask(harness.document, MaskId{"mask.brush"});
  const auto head_before    = harness.history.working_head();
  const auto preview_before = harness.preview_count;
  const auto count_before   = harness.document.PrimaryGrade()->MaskCount();
  const auto loaded         = harness.controller.SelectMask(harness.document.PrimaryGrade()->Id(),
                                                            MaskId{"mask.radial"}, harness.session);
  ASSERT_TRUE(loaded.accepted);
  EXPECT_FALSE(loaded.committed);
  EXPECT_FALSE(loaded.interactive_preview);
  EXPECT_FALSE(loaded.quality_requested);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_EQ(harness.preview_count, preview_before);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), count_before);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.radial"});
  const auto display = MakeRadialExistingOverlayDisplay(harness.mapping, radial,
                                                        DefaultMaskOverlayStyle(), QRectF());
  EXPECT_FALSE(display.handles.empty());
  EXPECT_EQ(OverlayFillCount(display), 0);

  const auto brush_loaded = harness.controller.SelectMask(harness.document.PrimaryGrade()->Id(),
                                                          MaskId{"mask.brush"}, harness.session);
  ASSERT_TRUE(brush_loaded.accepted);
  EXPECT_FALSE(brush_loaded.committed);
  EXPECT_FALSE(brush_loaded.interactive_preview);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_EQ(harness.preview_count, preview_before);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), count_before);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.brush"});
}

TEST(AnalyticMaskCreationTest, SelectedMaskCanBeEditedAfterWorkspaceReentry) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x     = 0.50f;
  radial.center_y     = 0.50f;
  radial.major_radius = 0.20f;
  radial.minor_radius = 0.16f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialCenter, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.33f, 0.58f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_EQ(harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->id,
            MaskId{"mask.radial"});
}

TEST(AnalyticMaskCreationTest, NodeDrawerDeleteRemovesExactMaskAndUndoRestoresSource) {
  AnalyticCreationHarness harness;
  RadialMaskSource        first;
  first.major_radius = 0.18f;
  first.minor_radius = 0.14f;
  RadialMaskSource second;
  second.center_x     = 0.62f;
  second.major_radius = 0.15f;
  second.minor_radius = 0.12f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.first"}, first);
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.second"}, second);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.first"}, harness.session)
          .accepted);
  const auto removed =
      harness.controller.RemoveMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.first"});
  ASSERT_TRUE(removed.accepted);
  EXPECT_TRUE(removed.committed);
  EXPECT_EQ(harness.document.PrimaryGrade()->FindMask(MaskId{"mask.first"}), nullptr);
  EXPECT_NE(harness.document.PrimaryGrade()->FindMask(MaskId{"mask.second"}), nullptr);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.second"});
  EXPECT_EQ(harness.controller.last_removed_mask_id(), MaskId{"mask.first"});

  const auto undone = harness.history.Undo();
  ASSERT_TRUE(undone.moved) << undone.error;
  ASSERT_TRUE(undone.selected_commit.has_value());
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(harness.document, BatchFromCommit(*undone.selected_commit),
                                     PipelineEditApplyDirection::Inverse, &error))
      << error;
  const auto* restored = harness.document.PrimaryGrade()->FindMask(MaskId{"mask.first"});
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 2u);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskAt(0).id, MaskId{"mask.first"});
  const auto* live = std::get_if<RadialMaskSource>(&restored->source);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(live->major_radius, first.major_radius);
}

TEST(AnalyticMaskCreationTest, DeletingLastMaskRestoresFullGradeCoverage) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.major_radius = 0.20f;
  radial.minor_radius = 0.14f;
  auto& mask   = grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.last"}, radial);
  mask.enabled = false;
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  EXPECT_EQ(MixAt(harness.mix, 1, 1), 0);
  ASSERT_TRUE(
      harness.controller.RemoveMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.last"})
          .accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  EXPECT_EQ(MixAt(harness.mix, 1, 1), 255);
}

TEST(AnalyticMaskCreationTest, DeletingUnselectedMaskPreservesSelection) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.major_radius = 0.18f;
  radial.minor_radius = 0.14f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.keep"}, radial);
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.other"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.keep"}, harness.session)
          .accepted);
  ASSERT_TRUE(
      harness.controller.RemoveMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.other"})
          .accepted);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.keep"});
  EXPECT_NE(harness.document.PrimaryGrade()->FindMask(MaskId{"mask.keep"}), nullptr);
}

TEST(AnalyticMaskCreationTest, DeletingMaskRejectsQueuedEditsAndDelayedFrames) {
  AnalyticCreationHarness harness;
  RadialMaskSource        moving;
  moving.center_x     = 0.50f;
  moving.center_y     = 0.50f;
  moving.major_radius = 0.20f;
  moving.minor_radius = 0.16f;
  RadialMaskSource keep;
  keep.center_x     = 0.20f;
  keep.center_y     = 0.20f;
  keep.major_radius = 0.12f;
  keep.minor_radius = 0.10f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.moving"}, moving);
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.keep"}, keep);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.moving"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialCenter, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.70f, 0.40f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  const auto delayed = harness.last_mix;
  ASSERT_TRUE(
      harness.controller.RemoveMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.moving"})
          .accepted);
  EXPECT_FALSE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->FindMask(MaskId{"mask.moving"}), nullptr);
  harness.mix.EvaluateFull(harness.document.PrimaryGrade()->Masks());
  EXPECT_NE(CopyMix(harness.mix), delayed);
  ExpectMixMatchesIndependent(harness.mix,
                              *harness.document.PrimaryGrade()->FindMask(MaskId{"mask.keep"}));
}

TEST(AnalyticMaskCreationTest, MaskDeleteWithViewerFocusDoesNotDeleteGrade) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.major_radius = 0.18f;
  radial.minor_radius = 0.12f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  const auto grade_id = harness.document.PrimaryGrade()->Id();
  ASSERT_TRUE(harness.controller.RemoveMask(grade_id, MaskId{"mask.radial"}).accepted);
  EXPECT_NE(harness.document.PrimaryGrade(), nullptr);
  EXPECT_EQ(harness.document.PrimaryGrade()->Id(), grade_id);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
}

TEST(AnalyticMaskCreationTest, GradientBoundaryDragPreservesOriginAndChangesTransition) {
  AnalyticCreationHarness  harness;
  LinearGradientMaskSource linear;
  linear.origin_x            = 0.50f;
  linear.origin_y            = 0.50f;
  linear.normal_x            = 0.0f;
  linear.normal_y            = 1.0f;
  linear.transition_distance = 0.24f;
  grade_mask_test::AddLinearGradientMask(harness.document, MaskId{"mask.linear"}, linear);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.linear"}, harness.session)
          .accepted);
  const Vector2 end{linear.origin_x, linear.origin_y + 0.40f};
  const auto    press = SampleOf(harness.mapping, ItemOf(harness.mapping, end), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::LinearEndBoundary, press, harness.pointer,
                           MaskSourceKind::LinearGradient)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(press, harness.pointer).accepted);
  const auto* live = std::get_if<LinearGradientMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.linear"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_NEAR(live->origin_x, linear.origin_x, 1.0e-5f);
  EXPECT_NEAR(live->origin_y, linear.origin_y, 1.0e-5f);
  EXPECT_GT(live->transition_distance, linear.transition_distance);
}

TEST(AnalyticMaskCreationTest, AnalyticControlCancelRestoresSourceWithoutHistory) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x     = 0.50f;
  radial.center_y     = 0.50f;
  radial.major_radius = 0.22f;
  radial.minor_radius = 0.16f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  const auto head_before = harness.history.working_head();
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.50f, 0.50f}), false);
  ASSERT_TRUE(
      harness.controller.BeginMaskMove(AnalyticMaskHandle::RadialCenter, press, harness.pointer,
                          MaskSourceKind::Radial)
          .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.30f, 0.60f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  ASSERT_TRUE(harness.controller.CancelMaskInput().accepted);
  EXPECT_EQ(harness.history.working_head(), head_before);
  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(live->center_x, radial.center_x);
  EXPECT_FLOAT_EQ(live->center_y, radial.center_y);
}

TEST(AnalyticMaskCreationTest, CoincidentRadialBoundariesKeepFeatherControlsReachable) {
  AnalyticCreationHarness harness;
  RadialMaskSource        radial;
  radial.center_x      = 0.50f;
  radial.center_y      = 0.50f;
  radial.major_radius  = 0.22f;
  radial.minor_radius  = 0.16f;
  radial.inner_feather = 0.0f;
  radial.outer_feather = 0.0f;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"}, radial);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  const auto inner_press = SampleOf(
      harness.mapping, ItemOf(harness.mapping, RadialNormalizedPoint(radial, 0.70f, 0.0f)), false);
  ASSERT_TRUE(
      harness.controller
          .BeginMaskMove(AnalyticMaskHandle::RadialInnerFeather, inner_press, harness.pointer,
                            MaskSourceKind::Radial)
          .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(inner_press, harness.pointer).accepted);
  ASSERT_TRUE(harness.controller.FinishMaskInput().accepted);
  ASSERT_TRUE(
      harness.controller
          .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.radial"}, harness.session)
          .accepted);
  MaskPointerIdentity outer_pointer = harness.pointer;
  outer_pointer.sequence_id         = 12;
  const auto outer_press            = SampleOf(
      harness.mapping, ItemOf(harness.mapping, RadialNormalizedPoint(radial, 1.35f, 0.0f)), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::RadialOuterFeather, outer_press, outer_pointer,
                           MaskSourceKind::Radial)
                  .accepted);
  ASSERT_TRUE(harness.controller.AppendMaskInput(outer_press, outer_pointer).accepted);
  const auto* live = std::get_if<RadialMaskSource>(
      &harness.document.PrimaryGrade()->FindMask(MaskId{"mask.radial"})->source);
  ASSERT_NE(live, nullptr);
  EXPECT_FLOAT_EQ(live->center_x, radial.center_x);
  EXPECT_FLOAT_EQ(live->center_y, radial.center_y);
  EXPECT_GT(live->inner_feather, 0.0f);
  EXPECT_GT(live->outer_feather, 0.0f);
}

TEST(AnalyticMaskCreationTest, MaskListDeletionChoosesNextThenPrevious) {
  const MaskId first{"mask.a"};
  const MaskId second{"mask.b"};
  const MaskId third{"mask.c"};
  const MaskId ids[] = {first, second, third};
  EXPECT_EQ(MaskIdAfterDeletion(ids, first, first), second);
  EXPECT_EQ(MaskIdAfterDeletion(ids, third, third), second);
  EXPECT_EQ(MaskIdAfterDeletion(ids, second, first), first);
  EXPECT_EQ(MaskIdAfterUndoRestore(first, MaskId{}), first);
  EXPECT_EQ(MaskIdAfterUndoRestore(first, second), second);
}

}  // namespace alcedo
