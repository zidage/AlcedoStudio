//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/op_base.hpp"
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

}  // namespace

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

TEST(PipelineHistoryApplierTest, ReplayAppliesLeftoverOrdinaryExposureOntoDefaultDocument) {
  auto        root  = CreateDefaultPipelineDocument();
  auto        graph = CommitGraph::CreateEmpty(21);
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "$operator_params";
  payload.after_value    = nlohmann::json{{"exposure", 0.85}};
  payload.after_enabled  = true;
  const auto  commit     = EditCommit::MakeEdit(graph.GetRootId(), std::nullopt, std::move(payload));
  std::string error;
  const auto  replayed   = ReplayPipelineDocumentFromRoot(root, {commit}, &error);
  ASSERT_TRUE(replayed.has_value()) << error;
  nlohmann::json exposure;
  ASSERT_TRUE(ReadEditorParameterJson(*replayed, test::ColorGradeFieldTarget("exposure"), &exposure,
                                      &error))
      << error;
  ASSERT_TRUE(exposure.contains("exposure_ev"));
  EXPECT_NEAR(exposure.at("exposure_ev").get<double>(), 0.85, 1e-5);
}

TEST(PipelineHistoryApplierTest, ReplayAppliesLeftoverLutOrdinaryPayloadOntoDefaultDocument) {
  auto        root  = CreateDefaultPipelineDocument();
  auto        graph = CommitGraph::CreateEmpty(22);
  OrdinaryEditPayload payload;
  payload.operator_type = OperatorType::LMT;
  payload.stage_name    = PipelineStageName::Color_Adjustment;
  payload.field_name    = "$operator_params";
  payload.after_value   = nlohmann::json{{"ocio_lmt", "D:/luts/teal_orange.cube"}};
  payload.after_enabled = true;
  const auto  commit    = EditCommit::MakeEdit(graph.GetRootId(), std::nullopt, std::move(payload));
  std::string error;
  const auto  replayed  = ReplayPipelineDocumentFromRoot(root, {commit}, &error);
  ASSERT_TRUE(replayed.has_value()) << error;
  auto* lmt = replayed->PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt());
  ASSERT_NE(lmt, nullptr);
  ASSERT_TRUE(lmt->ToJson().contains("cube_path"));
  EXPECT_EQ(lmt->ToJson().at("cube_path").get<std::string>(), "D:/luts/teal_orange.cube");
}

}  // namespace alcedo
