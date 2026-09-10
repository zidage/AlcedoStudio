//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QPointF>
#include <QRectF>
#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <variant>
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
#include "edit/mask/brush_placement.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/grade_mask_coverage.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"

namespace alcedo {
namespace {

constexpr Extent2D kRaster{64, 32};
constexpr float    kRadius = 8.0f;

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

[[nodiscard]] auto CopyMix(const GradeMaskCoverage& mix) -> std::vector<std::uint8_t> {
  const auto pixels = mix.Pixels();
  return {pixels.begin(), pixels.end()};
}

[[nodiscard]] auto OverlayFillCount(const MaskOverlayDisplay& display) -> int {
  return BuildMaskOverlaySceneGeometry(display, DefaultMaskOverlayStyle())
      .coverage_fill_vertex_count;
}

[[nodiscard]] auto BatchFromCommit(const EditCommit& commit) -> PipelineEditBatch {
  return PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
}

[[nodiscard]] auto LiveBrush(const PipelineDocument& document, const MaskId& id)
    -> const BrushMaskSource* {
  const auto* mask = document.PrimaryGrade()->FindMask(id);
  if (mask == nullptr) {
    return nullptr;
  }
  return std::get_if<BrushMaskSource>(&mask->source);
}

struct BrushCreationHarness {
  BrushCreationHarness()
      : graph(std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(17))),
        journal(std::make_shared<MiniGitJournal>()),
        history(graph, journal) {
    document = CreateDefaultPipelineDocument();
    mix.SetGeometry(kRaster, kRaster);
    controller.Bind(document, history);
    controller.SetInteractivePreview([this]() {
      ++preview_count;
      EvaluateMix();
      last_mix = CopyMix(mix);
    });
    (void)controller.SetBrushStrokeParameters(kRadius, 1.0f, 1.0f);
  }

  void EvaluateMix() {
    for (const auto& mask : document.PrimaryGrade()->Masks()) {
      if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
        mix.BindBrushSource(mask.id, *brush);
      }
    }
    mix.EvaluateFull(document.PrimaryGrade()->Masks());
  }

  auto ArmPaint() -> EditorMaskCreationResult {
    return controller.BeginCreation(MaskSourceKind::Brush, document.PrimaryGrade()->Id(), session);
  }

  auto PaintStroke(Vector2 from_normalized, Vector2 to_normalized, MaskPointerIdentity identity)
      -> EditorMaskCreationResult {
    const auto press  = SampleOf(mapping, ItemOf(mapping, from_normalized), false);
    const auto move   = SampleOf(mapping, ItemOf(mapping, to_normalized), true);
    EXPECT_TRUE(controller.BeginMaskInput(press, identity).accepted);
    EXPECT_TRUE(controller.AppendMaskInput(move, identity).accepted);
    return controller.FinishMaskInput();
  }

  auto AcknowledgePaint() -> bool {
    const auto id = controller.selected_mask_id();
    return controller.SelectMask(document.PrimaryGrade()->Id(), id, session).accepted &&
           controller.SetBrushTool(EditorBrushTool::Paint).accepted;
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

}  // namespace

TEST(AccumulatingBrushCreationTest, MultipleStrokesUseOneBrushMask) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);

  const auto first = harness.PaintStroke({0.20f, 0.40f}, {0.28f, 0.42f}, harness.pointer);
  ASSERT_TRUE(first.accepted);
  EXPECT_TRUE(first.committed);
  ASSERT_EQ(harness.document.PrimaryGrade()->MaskCount(), 1u);
  const auto mask_id = first.mask_id;
  const auto* brush  = LiveBrush(harness.document, mask_id);
  ASSERT_NE(brush, nullptr);
  EXPECT_EQ(brush->strokes.size(), 1u);
  EXPECT_FALSE(brush->asset_key.has_value());

  ASSERT_TRUE(harness.AcknowledgePaint());
  MaskPointerIdentity second_pointer{3, 1, 10};
  const auto second = harness.PaintStroke({0.55f, 0.60f}, {0.62f, 0.58f}, second_pointer);
  ASSERT_TRUE(second.accepted);
  EXPECT_TRUE(second.committed);
  EXPECT_EQ(second.mask_id, mask_id);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 1u);
  brush = LiveBrush(harness.document, mask_id);
  ASSERT_NE(brush, nullptr);
  ASSERT_EQ(brush->strokes.size(), 2u);
  EXPECT_NE(brush->strokes.front().id, brush->strokes.back().id);
  EXPECT_EQ(brush->strokes.front().mode, BrushStrokeMode::Paint);
  EXPECT_EQ(brush->strokes.back().mode, BrushStrokeMode::Paint);

  const auto display =
      MakeBrushExistingOverlayDisplay(harness.mapping, brush->placement_translation, QRectF());
  EXPECT_EQ(OverlayFillCount(display), 0);
  EXPECT_FALSE(display.handles.empty());
}

TEST(AccumulatingBrushCreationTest, SizeAndStrengthChangesPersistInStrokeSamples) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto press = SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.25f, 0.40f}), false);
  ASSERT_TRUE(harness.controller.BeginMaskInput(press, harness.pointer).accepted);
  ASSERT_TRUE(harness.controller.SetBrushStrokeParameters(4.0f, 0.5f, 1.0f).accepted);
  const auto move =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.45f, 0.40f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(move, harness.pointer).accepted);
  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  const auto* brush = LiveBrush(harness.document, finish.mask_id);
  ASSERT_NE(brush, nullptr);
  ASSERT_EQ(brush->strokes.size(), 1u);
  const auto samples = BrushStrokeSamples(brush->strokes.front());
  ASSERT_GE(samples.size(), 2u);
  EXPECT_FLOAT_EQ(samples.front().radius, kRadius);
  EXPECT_FLOAT_EQ(samples.front().strength, 1.0f);
  bool saw_boundary = false;
  for (const auto& sample : samples) {
    if (sample.radius == 4.0f && sample.strength == 0.5f) {
      saw_boundary = true;
    }
  }
  EXPECT_TRUE(saw_boundary);
}

TEST(AccumulatingBrushCreationTest, MovingExistingBrushUpdatesInteractivePixels) {
  BrushCreationHarness harness;
  auto mask = grade_mask_test::MakeParameterizedBrushMask(
      MaskId{"mask.brush"},
      {MakeBrushStroke(StrokeId{"stroke.1"}, BrushStrokeMode::Paint,
                       {{16.0f, 12.0f, kRadius, 1.0f, 1.0f}})});
  harness.document.PrimaryGrade()->AddMask(std::move(mask), 0);
  harness.EvaluateMix();
  const auto mix_before  = CopyMix(harness.mix);
  const auto head_before = harness.history.working_head();
  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.brush"},
                              harness.session)
                  .accepted);
  ASSERT_TRUE(harness.controller.SetBrushTool(EditorBrushTool::Move).accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{16.0f / 64.0f, 12.0f / 32.0f}),
               false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::BrushMove, press, harness.pointer)
                  .accepted);
  const auto moved =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{40.0f / 64.0f, 20.0f / 32.0f}),
               true);
  const auto preview = harness.controller.AppendMaskInput(moved, harness.pointer);
  ASSERT_TRUE(preview.accepted);
  EXPECT_TRUE(preview.interactive_preview);
  EXPECT_FALSE(preview.committed);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_GE(harness.preview_count, 1);
  EXPECT_NE(harness.last_mix, mix_before);
  const auto* live = LiveBrush(harness.document, MaskId{"mask.brush"});
  ASSERT_NE(live, nullptr);
  EXPECT_NE(live->placement_translation.x, 0.0f);
  EXPECT_EQ(live->strokes.size(), 1u);

  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  ASSERT_TRUE(harness.history.working_head().has_value());
  const auto* commit =
      harness.history.graph()->FindCommit(*harness.history.working_head());
  ASSERT_NE(commit, nullptr);
  const auto batch = BatchFromCommit(*commit);
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::SetBrushTranslation);
}

TEST(AccumulatingBrushCreationTest, BrushMoveDoesNotAppendStroke) {
  BrushCreationHarness harness;
  const auto original = MakeBrushStroke(StrokeId{"stroke.keep"}, BrushStrokeMode::Paint,
                                        {{10.0f, 8.0f, kRadius, 1.0f, 1.0f}});
  harness.document.PrimaryGrade()->AddMask(
      grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.brush"}, {original}), 0);
  const auto before_body = harness.document.PrimaryGrade()->BrushStrokes(MaskId{"mask.brush"}).front();
  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), MaskId{"mask.brush"},
                              harness.session)
                  .accepted);
  ASSERT_TRUE(harness.controller.SetBrushTool(EditorBrushTool::Move).accepted);
  const auto press = SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.20f, 0.30f}), false);
  ASSERT_TRUE(harness.controller
                  .BeginMaskMove(AnalyticMaskHandle::BrushMove, press, harness.pointer)
                  .accepted);
  const auto moved = SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.35f, 0.40f}), true);
  ASSERT_TRUE(harness.controller.AppendMaskInput(moved, harness.pointer).accepted);
  ASSERT_TRUE(harness.controller.FinishMaskInput().accepted);
  const auto strokes = harness.document.PrimaryGrade()->BrushStrokes(MaskId{"mask.brush"});
  ASSERT_EQ(strokes.size(), 1u);
  EXPECT_EQ(strokes.front().id, StrokeId{"stroke.keep"});
  EXPECT_TRUE(BrushStrokesShareSampleBody(before_body, strokes.front()));
}

TEST(AccumulatingBrushCreationTest, UndoLastStrokePreservesEarlierStrokes) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto first = harness.PaintStroke({0.20f, 0.40f}, {0.30f, 0.42f}, harness.pointer);
  ASSERT_TRUE(first.accepted);
  const auto mask_id = first.mask_id;
  const auto first_id =
      LiveBrush(harness.document, mask_id)->strokes.front().id;
  ASSERT_TRUE(harness.AcknowledgePaint());
  MaskPointerIdentity second_pointer{3, 1, 11};
  ASSERT_TRUE(harness.controller.SetBrushTool(EditorBrushTool::Erase).accepted);
  const auto second = harness.PaintStroke({0.22f, 0.41f}, {0.28f, 0.41f}, second_pointer);
  ASSERT_TRUE(second.accepted);
  ASSERT_EQ(LiveBrush(harness.document, mask_id)->strokes.size(), 2u);
  EXPECT_EQ(LiveBrush(harness.document, mask_id)->strokes.back().mode, BrushStrokeMode::Erase);

  const auto undone = harness.history.Undo();
  ASSERT_TRUE(undone.moved) << undone.error;
  ASSERT_TRUE(undone.selected_commit.has_value());
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(harness.document, BatchFromCommit(*undone.selected_commit),
                                     PipelineEditApplyDirection::Inverse, &error))
      << error;
  const auto* restored = LiveBrush(harness.document, mask_id);
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->strokes.size(), 1u);
  EXPECT_EQ(restored->strokes.front().id, first_id);
  EXPECT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->id, mask_id);

  const auto redone = harness.history.Redo();
  ASSERT_TRUE(redone.moved) << redone.error;
  ASSERT_TRUE(redone.selected_commit.has_value());
  ASSERT_TRUE(ApplyPipelineEditBatch(harness.document, BatchFromCommit(*redone.selected_commit),
                                     PipelineEditApplyDirection::Forward, &error))
      << error;
  const auto* replayed = LiveBrush(harness.document, mask_id);
  ASSERT_NE(replayed, nullptr);
  ASSERT_EQ(replayed->strokes.size(), 2u);
  EXPECT_EQ(replayed->strokes.front().id, first_id);
  EXPECT_EQ(replayed->strokes.back().mode, BrushStrokeMode::Erase);
}

TEST(AccumulatingBrushCreationTest, BrushReleaseCreatesNoHistoricalRasterFile) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto finish = harness.PaintStroke({0.40f, 0.50f}, {0.48f, 0.52f}, harness.pointer);
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  const auto* brush = LiveBrush(harness.document, finish.mask_id);
  ASSERT_NE(brush, nullptr);
  EXPECT_FALSE(brush->asset_key.has_value());
  EXPECT_TRUE(brush->descriptor.extent.Empty());
  ASSERT_TRUE(harness.history.working_head().has_value());
  const auto* commit =
      harness.history.graph()->FindCommit(*harness.history.working_head());
  ASSERT_NE(commit, nullptr);
  const auto payload = commit->GetPayloadJSON();
  const auto batch = PipelineEditBatch::FromJSON(payload);
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::AddMask);
  const auto dumped = payload.dump();
  EXPECT_EQ(dumped.find("asset_key"), std::string::npos);
  EXPECT_EQ(dumped.find("ReplaceMaskAsset"), std::string::npos);
  EXPECT_NE(dumped.find("strokes"), std::string::npos);
}

TEST(AccumulatingBrushCreationTest, HeaderBrushResumesSingleExistingBrush) {
  BrushCreationHarness harness;
  harness.document.PrimaryGrade()->AddMask(
      grade_mask_test::MakeParameterizedBrushMask(
          MaskId{"mask.existing"},
          {MakeBrushStroke(StrokeId{"stroke.keep"}, BrushStrokeMode::Paint,
                           {{8.0f, 8.0f, kRadius, 1.0f, 1.0f}})}),
      0);
  ASSERT_TRUE(harness.ArmPaint().accepted);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.existing"});
  EXPECT_EQ(harness.controller.brush_tool(), EditorBrushTool::Paint);
  const auto second = harness.PaintStroke({0.50f, 0.50f}, {0.58f, 0.52f}, harness.pointer);
  ASSERT_TRUE(second.accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 1u);
  EXPECT_EQ(second.mask_id, MaskId{"mask.existing"});
  EXPECT_EQ(LiveBrush(harness.document, MaskId{"mask.existing"})->strokes.size(), 2u);
}

TEST(AccumulatingBrushCreationTest, HeaderBrushWithMultipleExistingBrushesRequiresSelection) {
  BrushCreationHarness harness;
  harness.document.PrimaryGrade()->AddMask(
      grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.a"}), 0);
  harness.document.PrimaryGrade()->AddMask(
      grade_mask_test::MakeParameterizedBrushMask(MaskId{"mask.b"}), 1);
  const auto rejected = harness.ArmPaint();
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 2u);
  ASSERT_TRUE(harness.controller
                  .BeginCreation(MaskSourceKind::Brush, harness.document.PrimaryGrade()->Id(),
                                 harness.session, MaskId{"mask.b"})
                  .accepted);
  EXPECT_EQ(harness.controller.selected_mask_id(), MaskId{"mask.b"});
}

TEST(AccumulatingBrushCreationTest, DefaultBrushRadiusIsTwoPercentDiameterOfShorterEdge) {
  EXPECT_FLOAT_EQ(DefaultBrushRadiusReferencePixels(kRaster), 0.32f);
}

TEST(AccumulatingBrushCreationTest, CancelledFirstStrokeLeavesGradeUnchanged) {
  BrushCreationHarness harness;
  const auto           head_before = harness.history.working_head();
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto press =
      SampleOf(harness.mapping, ItemOf(harness.mapping, Vector2{0.30f, 0.40f}), false);
  ASSERT_TRUE(harness.controller.BeginMaskInput(press, harness.pointer).accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 1u);
  ASSERT_TRUE(harness.controller.CancelMaskInput().accepted);
  EXPECT_EQ(harness.document.PrimaryGrade()->MaskCount(), 0u);
  EXPECT_EQ(harness.history.working_head(), head_before);
  EXPECT_TRUE(harness.controller.selected_mask_id().Empty());
}

TEST(AccumulatingBrushCreationTest, OneShotMaskFieldEditsPublishTypedHistory) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto first = harness.PaintStroke({0.20f, 0.40f}, {0.28f, 0.42f}, harness.pointer);
  ASSERT_TRUE(first.accepted);
  const auto mask_id = first.mask_id;
  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);

  const auto opacity = harness.controller.ApplyMaskFieldValue("opacity", 0.4);
  ASSERT_TRUE(opacity.accepted);
  EXPECT_TRUE(opacity.committed);
  EXPECT_FLOAT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->opacity, 0.4f);
  ASSERT_TRUE(harness.history.working_head().has_value());
  auto batch = BatchFromCommit(
      *harness.history.graph()->FindCommit(*harness.history.working_head()));
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::SetMaskField);

  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);
  const auto invert = harness.controller.ApplyMaskFieldValue("invert", true);
  ASSERT_TRUE(invert.accepted);
  EXPECT_TRUE(invert.committed);
  EXPECT_TRUE(harness.document.PrimaryGrade()->FindMask(mask_id)->invert);
  batch = BatchFromCommit(
      *harness.history.graph()->FindCommit(*harness.history.working_head()));
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::SetMaskField);

  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);
  const auto name =
      harness.controller.ApplyMaskFieldValue("display_name", std::string{"Sky brush"});
  ASSERT_TRUE(name.accepted);
  EXPECT_TRUE(name.committed);
  EXPECT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->display_name, "Sky brush");

  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);
  const auto feather = harness.controller.ApplyMaskFieldValue("brush.feather", 2.5);
  ASSERT_TRUE(feather.accepted);
  EXPECT_TRUE(feather.committed);
  const auto* brush = LiveBrush(harness.document, mask_id);
  ASSERT_NE(brush, nullptr);
  EXPECT_FLOAT_EQ(brush->feather_radius, 2.5f);
  batch = BatchFromCommit(
      *harness.history.graph()->FindCommit(*harness.history.working_head()));
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::ReplaceMaskSource);
}

TEST(AccumulatingBrushCreationTest, MaskFieldDragSettlesOneCommit) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto first = harness.PaintStroke({0.20f, 0.40f}, {0.28f, 0.42f}, harness.pointer);
  ASSERT_TRUE(first.accepted);
  const auto mask_id = first.mask_id;
  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);

  ASSERT_TRUE(harness.controller.BeginMaskFieldEdit("opacity").accepted);
  const auto head_before = harness.history.working_head();
  const auto mid = harness.controller.ApplyMaskFieldValue("opacity", 0.3);
  ASSERT_TRUE(mid.accepted);
  EXPECT_FALSE(mid.committed);
  EXPECT_TRUE(mid.interactive_preview);
  EXPECT_FLOAT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->opacity, 0.3f);
  ASSERT_TRUE(harness.controller.ApplyMaskFieldValue("opacity", 0.6).accepted);
  const auto finish = harness.controller.FinishMaskInput();
  ASSERT_TRUE(finish.accepted);
  EXPECT_TRUE(finish.committed);
  EXPECT_FLOAT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->opacity, 0.6f);

  ASSERT_TRUE(harness.history.working_head().has_value());
  const auto* commit =
      harness.history.graph()->FindCommit(*harness.history.working_head());
  ASSERT_NE(commit, nullptr);
  const auto batch = BatchFromCommit(*commit);
  EXPECT_EQ(batch.operation_kind, PipelineEditOperationKind::SetMaskField);
  // Exactly one field commit beyond the AddMask history.
  EXPECT_NE(harness.history.working_head(), head_before);
  const auto parent = commit->GetFirstParentHash();
  ASSERT_TRUE(parent.has_value());
  const auto* parent_commit = harness.history.graph()->FindCommit(*parent);
  ASSERT_NE(parent_commit, nullptr);
  EXPECT_EQ(BatchFromCommit(*parent_commit).operation_kind,
            PipelineEditOperationKind::AddMask);
}

TEST(AccumulatingBrushCreationTest, MaskFieldEditCancelRestoresLiveValueWithoutHistory) {
  BrushCreationHarness harness;
  ASSERT_TRUE(harness.ArmPaint().accepted);
  const auto first = harness.PaintStroke({0.20f, 0.40f}, {0.28f, 0.42f}, harness.pointer);
  ASSERT_TRUE(first.accepted);
  const auto mask_id = first.mask_id;
  ASSERT_TRUE(harness.controller
                  .SelectMask(harness.document.PrimaryGrade()->Id(), mask_id, harness.session)
                  .accepted);

  const auto head_before = harness.history.working_head();
  ASSERT_TRUE(harness.controller.BeginMaskFieldEdit("opacity").accepted);
  ASSERT_TRUE(harness.controller.ApplyMaskFieldValue("opacity", 0.2).accepted);
  EXPECT_FLOAT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->opacity, 0.2f);
  ASSERT_TRUE(harness.controller.CancelMaskInput().accepted);
  EXPECT_FLOAT_EQ(harness.document.PrimaryGrade()->FindMask(mask_id)->opacity, 1.0f);
  EXPECT_EQ(harness.history.working_head(), head_before);
}

TEST(AccumulatingBrushCreationTest, MaskFieldEditRequiresSelectedMask) {
  BrushCreationHarness harness;
  EXPECT_FALSE(harness.controller.ApplyMaskFieldValue("opacity", 0.5).accepted);
  EXPECT_FALSE(harness.controller.BeginMaskFieldEdit("opacity").accepted);
}

}  // namespace alcedo
