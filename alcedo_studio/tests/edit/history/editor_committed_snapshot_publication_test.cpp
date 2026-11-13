//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The editor history publishes a committed pipeline graph snapshot of the image it holds after
// every change of its committed state, and never while an input sequence has left uncommitted
// values on the live document. Thumbnails and analysis read these snapshots through
// PipelineMgmtService::AcquireCommittedSnapshot.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/editor_parameter_write_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::ui {
namespace {

using alcedo::test::WithColorGradeTarget;

constexpr sl_element_id_t kElementId = 931;

/// Exposure of the current panel read the way the editor reads it; NaN when unavailable.
auto                      ExposureEv(const alcedo::PipelineDocument& document) -> double {
  std::string    error;
  const auto     target = alcedo::CompleteCurrentPanelParameterTarget(document, "exposure", &error);
  nlohmann::json json;
  if (!target.has_value() || !alcedo::ReadEditorParameterJson(document, *target, &json, &error) ||
      !json.contains("exposure_ev")) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return json.at("exposure_ev").get<double>();
}

class EditorCommittedSnapshotPublicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path();
    db_path_        = base / ("committed_publication_" + stamp + ".db");
    meta_path_      = base / ("committed_publication_" + stamp + ".json");
    journal_path_   = base / ("committed_publication_" + stamp + ".wal");
    project_        = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_,
                                                               alcedo::ProjectOpenMode::kCreateNew);
    pipelines_      = std::make_shared<alcedo::PipelineMgmtService>(project_->GetStorage());
    pipelines_->InitializeImageRoot(kElementId, alcedo::CreateDefaultPipelineDocument(), nullptr);
    pipeline_port_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_port_->SetServices(
        EditorSessionPipelineMappers{[pipelines = pipelines_]() { return pipelines; }, {}});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_port_);
    std::string error;
    handle_ = history_.Acquire(kElementId, &error);
    ASSERT_TRUE(handle_.valid) << error;
  }

  void TearDown() override {
    history_.Release(handle_);
    pipelines_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove(journal_path_, ec);
  }

  auto Committed() -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
    return pipelines_->AcquireCommittedSnapshot(kElementId);
  }

  /// Element pipeline JSON that older versions of the application read.
  auto ElementPipelineJson() -> nlohmann::json {
    const auto json = project_->GetStorage()->GetElementStore().GetPipelineJsonByElementId(kElementId);
    EXPECT_TRUE(json.has_value());
    return json.value_or(nlohmann::json{});
  }

  /// Commit an exposure edit that only the editor journal records.
  void CommitJournaledExposure() {
    std::string error;
    ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
        handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false}), &error))
        << error;
    ASSERT_TRUE(history_.CommitAdjustment(
        handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true}), &error))
        << error;
  }

  /// Release the image the way Close with Discard does.
  void DiscardClose() {
    history_.ReleaseDiscardingUnmaterialized(handle_);
    handle_ = {};
  }

  void Reopen() {
    std::string error;
    handle_ = history_.Acquire(kElementId, &error);
    ASSERT_TRUE(handle_.valid) << error;
  }

  /// Working document as the history published it after its last operation.
  auto Working() -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
    return pipeline_port_->CurrentPreview(kElementId);
  }

  /// Copy of the editor's CommitGraph, read through the history port.
  auto Graph() -> std::shared_ptr<const alcedo::CommitGraph> {
    std::shared_ptr<const alcedo::CommitGraph>      graph;
    std::shared_ptr<const alcedo::PipelineDocument> root_document;
    std::string                                     error;
    EXPECT_TRUE(history_.SnapshotHistorySource(handle_, &graph, &root_document, &error)) << error;
    return graph;
  }

  auto Head() -> alcedo::head_commit_hash_t {
    return Graph()->GetActiveVersionRef().head_commit_hash;
  }

  auto Chain() -> alcedo::transaction_chain_hash_t {
    const auto graph = Graph();
    return graph->ChainHashForHead(graph->GetActiveVersionRef().head_commit_hash);
  }

  /// True when the working document holds a value that is not committed: the save capture
  /// refuses exactly that state.
  auto HoldsUncommittedValues() -> bool {
    std::string error;
    const auto  capture = history_.CaptureSaveCheckpoint(handle_, &error);
    return capture == nullptr && error.find("unsettled") != std::string::npos;
  }

  /// Document of the active head, replayed from the immutable root the way every consumer
  /// rebuilds a committed state.
  auto HeadReplay() -> std::shared_ptr<alcedo::PipelineDocument> {
    std::shared_ptr<const alcedo::CommitGraph>      graph;
    std::shared_ptr<const alcedo::PipelineDocument> root_document;
    std::string                                     error;
    EXPECT_TRUE(history_.SnapshotHistorySource(handle_, &graph, &root_document, &error)) << error;
    if (!graph || !root_document) {
      return nullptr;
    }
    const alcedo::LoadedRootState root{alcedo::ClonePipelineDocument(*root_document), {}};
    auto                          replay = alcedo::BuildDocumentFromRoot(
        *graph, root, graph->GetActiveVersionRef().head_commit_hash, &error);
    EXPECT_NE(replay, nullptr) << error;
    return replay;
  }

  void CommitField(const std::string& field, const std::string& json) {
    std::string error;
    ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
        handle_, WithColorGradeTarget({field, json, false}), &error))
        << error;
    ASSERT_TRUE(
        history_.CommitAdjustment(handle_, WithColorGradeTarget({field, json, true}), &error))
        << error;
  }

  /// Leave a contrast preview on the working document, as a held slider or the debounce after an
  /// arrow key does.
  void OpenContrastPreview(const std::string& json) {
    std::string error;
    ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
        handle_, WithColorGradeTarget({"contrast", json, false}), &error))
        << error;
    ASSERT_TRUE(HoldsUncommittedValues());
  }

  /// The working document and the committed snapshot both equal the replay of the active head.
  void ExpectWorkingAndCommittedEqualHeadReplay(const char* move) {
    EXPECT_FALSE(HoldsUncommittedValues()) << move;
    const auto replay = HeadReplay();
    ASSERT_NE(replay, nullptr) << move;
    const auto committed = Committed();
    ASSERT_NE(committed, nullptr) << move;
    EXPECT_EQ(committed->Head(), Head()) << move;
    EXPECT_EQ(committed->Chain(), Chain()) << move;
    EXPECT_EQ(committed->Document().ToJson(), replay->ToJson())
        << move << ": the committed snapshot holds a value that no commit records";
    EXPECT_EQ(Working()->Document().ToJson(), replay->ToJson())
        << move << ": the working document differs from the replay of its head";
  }

  std::filesystem::path                        db_path_;
  std::filesystem::path                        meta_path_;
  std::filesystem::path                        journal_path_;
  std::unique_ptr<alcedo::ProjectService>      project_;
  std::shared_ptr<alcedo::PipelineMgmtService> pipelines_;
  std::shared_ptr<EditorSessionPipelinePort>   pipeline_port_;
  EditorSessionHistoryPort                     history_;
  alcedo::EditorHistoryGuardHandle             handle_{};
};

TEST_F(EditorCommittedSnapshotPublicationTest, OpeningTheImagePublishesItsCommittedState) {
  const auto committed = Committed();
  ASSERT_NE(committed, nullptr);
  EXPECT_TRUE(committed->IsCommitted());
  EXPECT_EQ(committed->ElementId(), kElementId);
  EXPECT_EQ(committed->Head(), Head());
  EXPECT_EQ(committed->Chain(), Chain());
  EXPECT_EQ(committed->Lineage(), Working()->Lineage());
  EXPECT_EQ(committed->Document().ToJson(), Working()->Document().ToJson());
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 0u)
      << "the image the editor holds is served from the editor's publication";
}

TEST_F(EditorCommittedSnapshotPublicationTest, SliderPreviewIsNotPublishedUntilItSettles) {
  std::string error;
  const auto  before = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false}), &error))
      << error;
  EXPECT_TRUE(HoldsUncommittedValues());
  EXPECT_NEAR(ExposureEv(Working()->Document()), 0.25, 1e-6);
  EXPECT_EQ(Committed(), before) << "a preview value is not a committed state";
  EXPECT_DOUBLE_EQ(ExposureEv(Committed()->Document()), ExposureEv(before->Document()));

  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", false}), &error))
      << error;
  EXPECT_EQ(Committed(), before);
  ASSERT_TRUE(history_.CommitAdjustment(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true}), &error))
      << error;
  EXPECT_FALSE(HoldsUncommittedValues());
  const auto settled = Committed();
  ASSERT_NE(settled, before);
  ASSERT_TRUE(settled->Head().has_value());
  EXPECT_EQ(settled->Head(), Head());
  EXPECT_EQ(settled->Chain(), Chain());
  EXPECT_NEAR(ExposureEv(settled->Document()), 0.75, 1e-6);

  ASSERT_TRUE(history_.Undo(handle_, &error)) << error;
  const auto undone = Committed();
  EXPECT_EQ(undone->Head(), before->Head());
  EXPECT_EQ(undone->Chain(), before->Chain());
  EXPECT_DOUBLE_EQ(ExposureEv(undone->Document()), ExposureEv(before->Document()));
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 0u);
}

TEST_F(EditorCommittedSnapshotPublicationTest, CancelledPreviewLeavesThePublishedStateUnchanged) {
  std::string error;
  const auto  before = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":1.5})", false}), &error))
      << error;
  bool live_changed = false;
  ASSERT_TRUE(history_.RestoreUnsettledPreview(handle_, &live_changed, &error)) << error;
  EXPECT_TRUE(live_changed);
  EXPECT_FALSE(HoldsUncommittedValues());
  EXPECT_EQ(Committed(), before) << "restoring the committed values publishes nothing new";
}

TEST_F(EditorCommittedSnapshotPublicationTest, OpenMaskInputIsUncommittedUntilItsSettle) {
  std::string  error;
  const auto   before   = Committed();
  const auto   grade_id = Working()->Document().DefaultGradeId();
  const MaskId mask_id{"mask.drag"};

  // First call: the input sequence writes a provisional mask and stays open, as a Mask drag does
  // between pointer samples.
  ASSERT_TRUE(history_.WithWorkingDocument(
      handle_,
      [&](alcedo::PipelineDocument& document, alcedo::MiniGitWorkingHistory&,
          const alcedo::IEditorHistoryPort::MaskSettle&, bool* input_open, std::string*) {
        auto* grade =
            dynamic_cast<alcedo::ColorGradeNodeModel*>(document.Graph().FindNode(grade_id));
        grade->AddMask(alcedo::grade_mask_test::MakeRadialMask(mask_id), 0);
        *input_open = true;
        return true;
      },
      &error))
      << error;
  EXPECT_TRUE(HoldsUncommittedValues())
      << "an open Mask input is uncommitted exactly like a slider preview";
  EXPECT_NE(Working()->Document().PrimaryGrade()->FindMask(mask_id), nullptr);
  EXPECT_EQ(Committed(), before);
  EXPECT_EQ(Committed()->Document().PrimaryGrade()->FindMask(mask_id), nullptr);

  // Second call: the pointer release settles the provisional mask as one commit and closes.
  ASSERT_TRUE(history_.WithWorkingDocument(
      handle_,
      [&](alcedo::PipelineDocument&                     document, alcedo::MiniGitWorkingHistory&,
          const alcedo::IEditorHistoryPort::MaskSettle& settle, bool* input_open,
          std::string* op_error) {
        const auto* grade = document.PrimaryGrade();
        const auto  batch = alcedo::MakeAddMaskBatch(
            grade_id, mask_id, alcedo::MaskModelToJson(*grade->FindMask(mask_id)), 0);
        const bool settled = settle(batch, op_error);
        *input_open        = false;
        return settled;
      },
      &error))
      << error;
  EXPECT_FALSE(HoldsUncommittedValues());
  const auto settled = Committed();
  ASSERT_NE(settled, before);
  EXPECT_EQ(settled->Head(), Head());
  EXPECT_NE(settled->Document().PrimaryGrade()->FindMask(mask_id), nullptr);
}

// The session policy refuses history moves while an input sequence is open. When a move still
// reaches the history port, the port restores the open preview first, so no uncommitted value
// stays on the document that the move publishes as committed.
TEST_F(EditorCommittedSnapshotPublicationTest, CommittedSnapshotNeverContainsAnUncommittedValue) {
  std::string error;
  CommitField("exposure", R"({"exposure":0.5})");
  const auto first = Head();
  ASSERT_TRUE(first.has_value());
  CommitField("exposure", R"({"exposure":1.0})");
  const auto second = Head();
  ASSERT_TRUE(second.has_value());

  OpenContrastPreview(R"({"contrast":40.0})");
  ASSERT_TRUE(history_.Undo(handle_, &error)) << error;
  EXPECT_EQ(Head(), first);
  ExpectWorkingAndCommittedEqualHeadReplay("Undo");

  OpenContrastPreview(R"({"contrast":-30.0})");
  ASSERT_TRUE(history_.Redo(handle_, &error)) << error;
  EXPECT_EQ(Head(), second);
  ExpectWorkingAndCommittedEqualHeadReplay("Redo");

  OpenContrastPreview(R"({"contrast":25.0})");
  ASSERT_TRUE(history_.MoveHeadToCommit(handle_, *first, &error)) << error;
  EXPECT_EQ(Head(), first);
  ExpectWorkingAndCommittedEqualHeadReplay("MoveHead");
}

TEST_F(EditorCommittedSnapshotPublicationTest, SaveCheckpointMatchesHeadReplayAfterHistoryMoves) {
  std::string error;
  CommitField("exposure", R"({"exposure":0.5})");
  const auto first = Head();
  CommitField("exposure", R"({"exposure":1.0})");

  OpenContrastPreview(R"({"contrast":40.0})");
  ASSERT_TRUE(history_.Undo(handle_, &error)) << error;
  OpenContrastPreview(R"({"contrast":-30.0})");
  ASSERT_TRUE(history_.Redo(handle_, &error)) << error;
  OpenContrastPreview(R"({"contrast":25.0})");
  ASSERT_TRUE(history_.MoveHeadToCommit(handle_, *first, &error)) << error;

  const auto capture = history_.CaptureSaveCheckpoint(handle_, &error);
  ASSERT_NE(capture, nullptr) << error;
  EXPECT_EQ(capture->working_head, first);
  const auto& stored = capture->materialization.image_state.serialized_pipeline_state;
  ASSERT_TRUE(stored.has_value());
  const auto replay = HeadReplay();
  ASSERT_NE(replay, nullptr);
  EXPECT_EQ(*stored, alcedo::MakeEditorSerializedPipelineState(Graph()->GetRootId(), first, Chain(),
                                                               *replay))
      << "the Save checkpoint of HEAD must hold the replay of HEAD";
}

TEST_F(EditorCommittedSnapshotPublicationTest, ReleasedImageIsServedFromStorageWhenItDiffers) {
  std::string error;
  const auto  opened = Committed();
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false}), &error))
      << error;
  ASSERT_TRUE(history_.CommitAdjustment(
      handle_, WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true}), &error))
      << error;
  const auto journaled = Committed();
  ASSERT_TRUE(journaled->Head().has_value());

  // Release without a save: the commit exists only in the journal, so storage still holds the
  // root state and the released entry no longer matches it.
  history_.Release(handle_);
  handle_           = {};
  const auto stored = Committed();
  EXPECT_FALSE(stored->Head().has_value());
  EXPECT_EQ(pipelines_->CommittedSnapshotStorageLoadCount(), 1u);
  EXPECT_EQ(stored->Chain(), opened->Chain());
  EXPECT_DOUBLE_EQ(ExposureEv(stored->Document()), ExposureEv(opened->Document()));
}

TEST_F(EditorCommittedSnapshotPublicationTest,
       DiscardCloseLeavesElementPipelineJsonAtTheMaterializedState) {
  CommitJournaledExposure();
  const auto discarded = Committed();
  ASSERT_TRUE(discarded->Head().has_value());

  DiscardClose();

  const auto materialized = Committed();
  EXPECT_FALSE(materialized->Head().has_value());
  ASSERT_NE(materialized->Document().ToJson(), discarded->Document().ToJson());
  EXPECT_EQ(ElementPipelineJson(), materialized->Document().ToJson())
      << "the element pipeline JSON received the discarded commit";
}

TEST_F(EditorCommittedSnapshotPublicationTest, DiscardCloseThenReopenShowsTheMaterializedState) {
  const auto opened = Committed();
  CommitJournaledExposure();
  ASSERT_NE(ExposureEv(Committed()->Document()), ExposureEv(opened->Document()));

  DiscardClose();
  Reopen();

  EXPECT_EQ(Head(), opened->Head()) << "the discarded commit came back from the journal";
  EXPECT_DOUBLE_EQ(ExposureEv(Working()->Document()), ExposureEv(opened->Document()));
  EXPECT_DOUBLE_EQ(ExposureEv(Committed()->Document()), ExposureEv(opened->Document()));

  // The journal was dropped, not only skipped by this acquire: storage keeps the root state.
  history_.Release(handle_);
  handle_ = {};
  EXPECT_FALSE(Committed()->Head().has_value());
}

TEST_F(EditorCommittedSnapshotPublicationTest, SavedCloseWritesTheSavedStateToElementPipelineJson) {
  CommitJournaledExposure();
  const auto saved = Committed();
  const auto graph = Graph();
  auto       persisted_graph = *graph;
  const auto document        = HeadReplay();
  ASSERT_NE(document, nullptr);
  std::string error;
  ASSERT_TRUE(pipelines_->PersistEditorHistory(persisted_graph, graph->GetImageEditState(),
                                                *document, &error))
      << error;

  history_.Release(handle_);
  handle_ = {};

  EXPECT_EQ(ElementPipelineJson(), saved->Document().ToJson());
}

}  // namespace
}  // namespace alcedo::ui
