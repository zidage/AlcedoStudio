//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/op_base.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

void RoundTripOwner(PipelineDocument* document, const EditorParameterTarget& target,
                    const nlohmann::json& patch) {
  nlohmann::json before;
  std::string    error;
  ASSERT_TRUE(ReadEditorParameterJson(*document, target, &before, &error)) << error;
  ASSERT_TRUE(ApplyEditorParameterPatch(*document, target, patch, &error)) << error;
  nlohmann::json after;
  ASSERT_TRUE(ReadEditorParameterJson(*document, target, &after, &error)) << error;
  ASSERT_TRUE(ApplyEditorParameterPatch(*document, target, before, &error)) << error;
  const auto start = CanonicalPipelineDocumentJson(*document);
  const auto batch =
      MakeSetParameterBatch(target, before, after, true, true, std::string{target.node_id.Value()});
  ASSERT_TRUE(ApplyPipelineEditBatch(*document, batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  ASSERT_TRUE(ApplyPipelineEditBatch(*document, batch, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(CanonicalPipelineDocumentJson(*document), start);
  ASSERT_TRUE(ApplyPipelineEditBatch(*document, batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  ASSERT_TRUE(ApplyPipelineEditBatch(*document, batch, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(CanonicalPipelineDocumentJson(*document), start);
}

auto TwoFieldPaste(const PipelineDocument& document) -> PipelineEditBatch {
  const auto exposure = test::ColorGradeFieldTarget("exposure");
  const auto contrast = test::ColorGradeFieldTarget("contrast");
  nlohmann::json exposure_json;
  nlohmann::json contrast_json;
  std::string    error;
  EXPECT_TRUE(ReadEditorParameterJson(document, exposure, &exposure_json, &error)) << error;
  EXPECT_TRUE(ReadEditorParameterJson(document, contrast, &contrast_json, &error)) << error;
  SetParameterChange first;
  first.target         = ToPipelineParameterTarget(exposure);
  first.before_value   = exposure_json;
  first.after_value    = exposure_json;
  first.after_value["exposure_ev"] = 3.0;
  first.before_enabled = true;
  first.after_enabled  = true;
  SetParameterChange second;
  second.target         = ToPipelineParameterTarget(contrast);
  second.before_value   = contrast_json;
  second.after_value    = contrast_json;
  if (second.after_value.contains("contrast") && second.after_value.at("contrast").is_number()) {
    second.after_value["contrast"] = second.after_value.at("contrast").get<double>() + 0.25;
  } else {
    second.after_value["contrast"] = 0.25;
  }
  second.before_enabled = true;
  second.after_enabled  = true;
  return PipelineEditBatch::Make(PipelineEditOperationKind::Paste, {first, second},
                                 PresentationKeyForOperation(PipelineEditOperationKind::Paste));
}

auto BatchFromCommit(const EditCommit& commit) -> PipelineEditBatch {
  return PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
}

}  // namespace

TEST(PipelineHistoryApplierTest, FirstChangeMismatchLeavesDocumentUnchanged) {
  auto        document = CreateDefaultPipelineDocument();
  const auto  start    = CanonicalPipelineDocumentJson(document);
  const auto  target   = test::ColorGradeFieldTarget("exposure");
  const auto  batch =
      MakeSetParameterBatch(target, nlohmann::json{{"exposure_ev", 99.0}},
                            nlohmann::json{{"exposure_ev", 2.0}}, true, true, "Grade");
  std::string error;
  EXPECT_FALSE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error));
  EXPECT_NE(error.find("expected current side"), std::string::npos);
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), start);
}

TEST(PipelineHistoryApplierTest, GraphFinalValidationRestoresExactNodesAndEdges) {
  auto        document = CreateDefaultPipelineDocument();
  const auto  start    = CanonicalPipelineDocumentJson(document);
  auto        change =
      CaptureAddColorGradeChange(document, NodeId{"drt"}, NodeId{"grade.look"});
  auto        batch = MakeAddColorGradeBatch(std::move(change));
  std::string error;
  PipelineHistoryApplyContext context;
  context.after_successful_change = [&](std::size_t) {
    document.Graph().Connect(NodeId{"develop"}, PortId{"image"}, NodeId{"drt"}, PortId{"image"});
  };
  EXPECT_FALSE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error,
                                      context));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), start);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.look"}), nullptr);
}

TEST(PipelineHistoryApplierTest, ParameterForwardInverseRestoresDocumentHash) {
  auto        document = CreateDefaultPipelineDocument();
  std::string error;
  auto        crop = CompleteCurrentPanelParameterTarget(document, "crop_rotate", &error);
  ASSERT_TRUE(crop.has_value()) << error;
  RoundTripOwner(&document, *crop, {{"rotation_degrees", 12.0}});

  auto develop = CompleteCurrentPanelParameterTarget(document, "color_temp", &error);
  ASSERT_TRUE(develop.has_value()) << error;
  nlohmann::json develop_json;
  ASSERT_TRUE(ReadEditorParameterJson(document, *develop, &develop_json, &error)) << error;
  ASSERT_TRUE(develop_json.contains("custom_cct")) << develop_json.dump();
  RoundTripOwner(&document, *develop, {{"custom_cct", 7200.0}, {"wb_mode", "custom"}});

  RoundTripOwner(&document, test::ColorGradeFieldTarget("exposure"), {{"exposure_ev", 2.25}});
  RoundTripOwner(&document, test::DrtPostFieldTarget("clarity"), {{"clarity", 18.0}});
}

TEST(PipelineHistoryApplierTest, LaterChangeFailureReversesEarlierBatchChanges) {
  auto        document = CreateDefaultPipelineDocument();
  const auto  start    = CanonicalPipelineDocumentJson(document);
  auto        batch    = TwoFieldPaste(document);
  std::string error;
  PipelineHistoryApplyContext context;
  context.after_successful_change = [](std::size_t applied) {
    if (applied == 1) {
      throw std::runtime_error("injected later-change failure");
    }
  };
  EXPECT_FALSE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error,
                                      context));
  EXPECT_NE(error.find("injected later-change failure"), std::string::npos);
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), start);
}

TEST(PipelineHistoryApplierTest, ConcurrentRenderCannotObservePartialTypedBatch) {
  auto       document = CreateDefaultPipelineDocument();
  const auto start    = CanonicalPipelineDocumentJson(document);
  auto       batch    = TwoFieldPaste(document);
  std::timed_mutex render_lock;
  std::string mid_hash;
  std::promise<void> after_first;
  PipelineHistoryApplyContext context;
  context.after_successful_change = [&](std::size_t applied) {
    if (applied != 1) {
      return;
    }
    mid_hash = CanonicalPipelineDocumentJson(document);
    after_first.set_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  };
  auto worker = std::async(std::launch::async, [&] {
    std::lock_guard lock(render_lock);
    std::string     error;
    return ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error,
                                  context);
  });
  after_first.get_future().wait();
  std::unique_lock attempt(render_lock, std::defer_lock);
  EXPECT_FALSE(attempt.try_lock_for(std::chrono::milliseconds(40)));
  EXPECT_TRUE(worker.get());
  std::lock_guard done(render_lock);
  const auto      final_hash = CanonicalPipelineDocumentJson(document);
  EXPECT_NE(final_hash, start);
  EXPECT_NE(final_hash, mid_hash);
}

TEST(PipelineHistoryApplierTest, HeadPublishFailureRevokesOnlyNewJournalTail) {
  const auto dir =
      std::filesystem::path{"build"} / "tmp" / "node_history" / "head_publish_failure";
  std::filesystem::create_directories(dir);
  const auto journal_path = dir / "image.wal";
  std::error_code ec;
  std::filesystem::remove(journal_path, ec);

  auto journal = std::make_shared<MiniGitJournal>(journal_path);
  auto graph   = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(7));
  MiniGitWorkingHistory history(graph, journal);

  const auto target = test::ColorGradeFieldTarget("exposure");
  const auto first  = MakeSetParameterBatch(target, {{"exposure_ev", 1.5}}, {{"exposure_ev", 2.0}},
                                           true, true, "Grade");
  const auto first_append = history.AppendEdit(first);
  ASSERT_TRUE(first_append.committed) << first_append.error;
  ASSERT_EQ(journal->records().size(), 1u);
  const auto first_head = history.working_head();

  const auto second = MakeSetParameterBatch(target, {{"exposure_ev", 2.0}}, {{"exposure_ev", 2.5}},
                                            true, true, "Grade");
  const auto prepared = history.PrepareAppendEdit(second);
  ASSERT_TRUE(prepared.ready) << prepared.error;
  ASSERT_TRUE(graph->InsertCommit(prepared.commit));
  const auto published = history.PublishPreparedEdit(prepared);
  EXPECT_FALSE(published.committed);
  EXPECT_EQ(journal->records().size(), 1u);
  EXPECT_EQ(history.working_head(), first_head);
}

TEST(PipelineHistoryApplierTest, ReplayAppliesTypedBatchExposureOntoDefaultDocument) {
  auto root  = CreateDefaultPipelineDocument();
  auto graph = CommitGraph::CreateEmpty(21);

  const auto target = test::ColorGradeFieldTarget("exposure");
  nlohmann::json before_json;
  std::string error;
  ASSERT_TRUE(ReadEditorParameterJson(root, target, &before_json, &error)) << error;
  auto after_json = before_json;
  after_json["exposure_ev"] = 0.85;

  SetParameterChange change;
  change.target         = ToPipelineParameterTarget(target);
  change.before_value   = before_json;
  change.after_value    = after_json;
  change.before_enabled = true;
  change.after_enabled  = true;

  PipelineEditBatch batch;
  batch.operation_kind   = PipelineEditOperationKind::SetParameter;
  batch.presentation_key = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));
  const auto commit = EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt, std::move(batch));
  const auto replayed = ReplayPipelineDocumentFromRoot(root, {commit}, &error);
  ASSERT_TRUE(replayed.has_value()) << error;
  nlohmann::json exposure;
  ASSERT_TRUE(ReadEditorParameterJson(*replayed, test::ColorGradeFieldTarget("exposure"), &exposure,
                                      &error))
      << error;
  ASSERT_TRUE(exposure.contains("exposure_ev"));
  EXPECT_NEAR(exposure.at("exposure_ev").get<double>(), 0.85, 1e-5);
}

TEST(PipelineHistoryApplierTest, ReplayRejectsNonBatchPayload) {
  auto root  = CreateDefaultPipelineDocument();
  auto graph = CommitGraph::CreateEmpty(22);
  nlohmann::json non_batch_payload = {{"operator_type", 1}, {"exposure", 0.85}};
  nlohmann::json commit_json;
  commit_json["root_id"] = graph.GetRootId().ToString();
  commit_json["first_parent_hash"] = "";
  commit_json["second_parent_hash"] = "";
  commit_json["created_at_ns"] = 100;
  commit_json["kind"] = "edit";
  commit_json["edit_payload"] = non_batch_payload;
  EXPECT_THROW(EditCommit::FromJSON(commit_json), std::runtime_error);
}

TEST(PipelineHistoryApplierTest, MaskAddCreatesOneCommitAndUndoRedoRestoresIt) {
  const auto dir = std::filesystem::path{"build/tmp/mask_history"} / "add_mask";
  std::filesystem::create_directories(dir);
  const auto journal_path = dir / "image.wal";
  std::error_code ec;
  std::filesystem::remove(journal_path, ec);

  auto journal = std::make_shared<MiniGitJournal>(journal_path);
  auto graph   = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(31));
  MiniGitWorkingHistory history(graph, journal);

  auto document = CreateDefaultPipelineDocument();
  RadialMaskSource radial;
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  auto       mask = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}, radial);
  const auto add =
      MakeAddMaskBatch(document.PrimaryGrade()->Id(), mask.id, MaskModelToJson(mask), 0);
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, add, PipelineEditApplyDirection::Forward, &error))
      << error;
  const auto append = history.AppendEdit(add);
  ASSERT_TRUE(append.committed) << append.error;
  EXPECT_EQ(graph->CommitCount(), 1u);
  EXPECT_EQ(journal->records().size(), 1u);
  ASSERT_EQ(document.PrimaryGrade()->MaskCount(), 1u);
  const auto* added = document.PrimaryGrade()->FindMask(MaskId{"mask.radial"});
  ASSERT_NE(added, nullptr);
  EXPECT_TRUE(std::holds_alternative<RadialMaskSource>(added->source));

  const auto undone = history.Undo();
  ASSERT_TRUE(undone.moved) << undone.error;
  ASSERT_TRUE(undone.selected_commit.has_value());
  ASSERT_TRUE(ApplyPipelineEditBatch(document, BatchFromCommit(*undone.selected_commit),
                                     PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(document.PrimaryGrade()->FindMask(MaskId{"mask.radial"}), nullptr);
  EXPECT_EQ(journal->records().size(), 2u);

  const auto redone = history.Redo();
  ASSERT_TRUE(redone.moved) << redone.error;
  ASSERT_TRUE(redone.selected_commit.has_value());
  ASSERT_TRUE(ApplyPipelineEditBatch(document, BatchFromCommit(*redone.selected_commit),
                                     PipelineEditApplyDirection::Forward, &error))
      << error;
  const auto* restored = document.PrimaryGrade()->FindMask(MaskId{"mask.radial"});
  ASSERT_NE(restored, nullptr);
  const auto* restored_radial = std::get_if<RadialMaskSource>(&restored->source);
  ASSERT_NE(restored_radial, nullptr);
  EXPECT_FLOAT_EQ(restored_radial->major_radius, 0.3f);
}

}  // namespace alcedo
