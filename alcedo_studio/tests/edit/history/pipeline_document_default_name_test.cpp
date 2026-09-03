//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"

namespace alcedo {

TEST(PipelineDocumentDefaultName, NewDocumentStoresFirstNameAndNextCounter) {
  const auto document = CreateDefaultPipelineDocument();
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  EXPECT_EQ(document.PrimaryGrade()->DisplayName(), "Color Grade 1");
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  ASSERT_TRUE(document.ToJson().contains("next_color_grade_name_number"));
  EXPECT_EQ(document.ToJson().at("next_color_grade_name_number"), 2u);

  const auto reopened = PipelineDocument::FromJson(document.ToJson());
  EXPECT_EQ(reopened.PrimaryGrade()->DisplayName(), "Color Grade 1");
  EXPECT_EQ(reopened.NextColorGradeNameNumber(), 2u);
}

TEST(PipelineDocumentDefaultName, SuccessfulAddsUseIncreasingNamesAndRemovalDoesNotReuse) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.third"}).empty());
  ASSERT_EQ(document.NextColorGradeNameNumber(), 4u);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.second"})->DisplayName(), "Color Grade 2");
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.third"})->DisplayName(), "Color Grade 3");

  EXPECT_TRUE(RenameColorGrade(document, NodeId{"grade.second"}, "Renamed").empty());
  EXPECT_TRUE(ReconnectColorGrade(document, NodeId{"grade.second"}, NodeId{"develop"},
                                  NodeId{"grade.primary"})
                  .empty());
  EXPECT_EQ(document.NextColorGradeNameNumber(), 4u);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.second"})->DisplayName(), "Renamed");
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.third"})->DisplayName(), "Color Grade 3");

  EXPECT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.second"}).empty());
  EXPECT_EQ(document.NextColorGradeNameNumber(), 4u);
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.fourth"}).empty());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.fourth"})->DisplayName(), "Color Grade 4");
  EXPECT_EQ(document.NextColorGradeNameNumber(), 5u);
}

TEST(PipelineDocumentDefaultName, FailedAddDoesNotConsumeCounter) {
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = document.NextColorGradeNameNumber();

  const auto errors   = AddCleanColorGrade(document, NodeId{"develop"}, NodeId{"grade.failed"});
  ASSERT_FALSE(errors.empty());
  EXPECT_EQ(document.NextColorGradeNameNumber(), before);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.failed"}), nullptr);

  const auto duplicate = AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.primary"});
  ASSERT_FALSE(duplicate.empty());
  EXPECT_EQ(document.NextColorGradeNameNumber(), before);
}

TEST(PipelineDocumentDefaultName, TypedAddReplaysExactCounterAndNameAcrossUndoRedoAndReplay) {
  auto document = CreateDefaultPipelineDocument();
  auto change   = CaptureAddColorGradeChange(document, NodeId{"drt"}, NodeId{"grade.replay"});
  EXPECT_EQ(change.before_next_color_grade_name_number, 2u);
  EXPECT_EQ(change.after_next_color_grade_name_number, 3u);
  EXPECT_EQ(change.node.at("display_name"), "Color Grade 2");

  const auto batch          = MakeAddColorGradeBatch(change);
  const auto restored_batch = PipelineEditBatch::FromJSON(batch.CanonicalJSON());
  EXPECT_EQ(restored_batch.CanonicalJSON().dump(), batch.CanonicalJSON().dump());

  std::string error;
  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, restored_batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_EQ(document.NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.replay"})->DisplayName(), "Color Grade 2");

  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, restored_batch, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.replay"}), nullptr);

  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, restored_batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  auto       graph  = CommitGraph::CreateEmpty(205);
  const auto commit = EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt, restored_batch);
  const auto replayed =
      ReplayPipelineDocumentFromRoot(CreateDefaultPipelineDocument(), {commit}, &error);
  ASSERT_TRUE(replayed.has_value()) << error;
  EXPECT_EQ(replayed->NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(replayed->Graph().FindNode(NodeId{"grade.replay"})->DisplayName(), "Color Grade 2");
}

TEST(PipelineDocumentDefaultName, StoredNodeInsertionLeavesCounterUnchanged) {
  auto document = CreateDefaultPipelineDocument();
  auto change   = CaptureAddColorGradeChange(document, NodeId{"drt"}, NodeId{"grade.pasted"});
  change.before_next_color_grade_name_number = document.NextColorGradeNameNumber();
  change.after_next_color_grade_name_number  = document.NextColorGradeNameNumber();
  const auto  batch                          = MakeAddColorGradeBatch(std::move(change));

  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  ASSERT_TRUE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
}

TEST(PipelineDocumentDefaultName, InvalidSerializedCounterAndOldFormatAreRejected) {
  auto document                        = CreateDefaultPipelineDocument();
  auto json                            = document.ToJson();
  json["next_color_grade_name_number"] = 0;
  EXPECT_THROW(PipelineDocument::FromJson(json), std::runtime_error);

  json                   = document.ToJson();
  json["format_version"] = 4;
  EXPECT_THROW(PipelineDocument::FromJson(json), std::runtime_error);

  EXPECT_THROW(document.SetNextColorGradeNameNumber(0), std::invalid_argument);
  document.SetNextColorGradeNameNumber(std::numeric_limits<std::uint64_t>::max());
  const auto exhausted = AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.exhausted"});
  ASSERT_FALSE(exhausted.empty());
  EXPECT_EQ(document.NextColorGradeNameNumber(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.exhausted"}), nullptr);
  EXPECT_THROW(document.ConsumeNextColorGradeNameNumber(), std::overflow_error);
}

}  // namespace alcedo
