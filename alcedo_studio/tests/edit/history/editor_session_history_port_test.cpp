//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <variant>

#include "json.hpp"
#include "app/adjustment_transfer_service.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/editor_history_types.hpp"
#include "ui/alcedo_main/album_backend/editor_history_commit_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::ui {
namespace {

auto MakeMiniGitPipelineGuard(sl_element_id_t element_id)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_                  = guard->commit_graph_->GetRootId();
  guard->root_document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::ClonePipelineDocument(*guard->document_));
  return guard;
}

using alcedo::test::WithColorGradeTarget;

auto ColorGradeTargetForField(const std::string& field, std::string node_id = "grade.primary")
    -> alcedo::EditorParameterTarget {
  alcedo::EditorParameterTarget target;
  target.owner_kind              = alcedo::EditorParameterOwnerKind::ColorGrade;
  target.node_id                 = alcedo::NodeId{node_id};
  target.adjustment_instance_id  = alcedo::AdjustmentInstanceId{node_id + "." + field};
  target.field_key               = field;
  return target;
}

auto CommitSettled(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                   const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview = WithColorGradeTarget({field, after_json, false});
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  alcedo::EditorAdjustmentPatch settled = WithColorGradeTarget({field, after_json, true});
  return port.CommitAdjustment(handle, settled, error);
}

auto PatchValue(const alcedo::EditorRenderAdjustmentSnapshot& snapshot, const std::string& field)
    -> std::string {
  for (const auto& patch : snapshot.patches) {
    if (patch.field_key == field) return patch.params_json;
  }
  return {};
}

auto ReadJsonNumber(const std::string& serialized, const std::string& key)
    -> std::optional<double> {
  if (serialized.empty()) {
    return std::nullopt;
  }
  try {
    return nlohmann::json::parse(serialized).at(key).get<double>();
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

auto PatchNumber(const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                 const std::string& field) -> std::optional<double> {
  return ReadJsonNumber(PatchValue(snapshot, field), field);
}

auto MakeExposureTransferPackage(double exposure) -> alcedo::AdjustmentTransferPackage {
  return alcedo::test::MakeExposureTransferPackage(exposure);
}

auto MakeLutTransferPackage(std::string lut_path) -> alcedo::AdjustmentTransferPackage {
  return alcedo::test::MakeLutTransferPackage(std::move(lut_path));
}

auto LiveLutPath(const std::shared_ptr<alcedo::PipelineGuard>& guard) -> std::string {
  if (!guard || !guard->pipeline_) {
    return {};
  }
  const auto entry =
      guard->pipeline_->GetStage(alcedo::PipelineStageName::Color_Adjustment)
          .GetOperator(alcedo::OperatorType::LMT);
  if (!entry.has_value() || entry.value() == nullptr || !entry.value()->op_) {
    return {};
  }
  const auto params = entry.value()->op_->GetParams();
  if (!params.contains("ocio_lmt") || !params["ocio_lmt"].is_string()) {
    return {};
  }
  return params["ocio_lmt"].get<std::string>();
}

auto LutPathFromSnapshot(const alcedo::EditorRenderAdjustmentSnapshot& snapshot) -> std::string {
  const auto serialized = PatchValue(snapshot, "lut");
  if (serialized.empty()) {
    return {};
  }
  try {
    const auto params = nlohmann::json::parse(serialized);
    if (params.contains("ocio_lmt") && params["ocio_lmt"].is_string()) {
      return params["ocio_lmt"].get<std::string>();
    }
  } catch (const nlohmann::json::exception&) {
  }
  return {};
}

/// Mirrors AdjustmentTransferController::PasteViaMiniGit: root-relative Version,
/// rebuild live pipeline, persist graph, optionally request checkpoint writeback.
auto LibraryPasteThenRelease(alcedo::PipelineMgmtService& pipeline_service,
                             sl_element_id_t element_id,
                             const alcedo::AdjustmentTransferPackage& package,
                             bool writeback_after_persist, std::string* error) -> bool {
  auto guard = pipeline_service.LoadEditorPipeline(element_id);
  if (!guard || !guard->commit_graph_ || !guard->pipeline_ || !guard->root_document_) {
    if (error) *error = "Library paste requires a loaded editor pipeline";
    return false;
  }
  const auto expected = guard->commit_graph_->GetImageEditState();
  const auto pasted   = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
      *guard->commit_graph_, *guard->root_document_, package, "Pasted Adjustments");
  if (!pasted.pasted) {
    if (error) *error = pasted.error.empty() ? "Library paste failed" : pasted.error;
    pipeline_service.SavePipeline(guard);
    return false;
  }
  if (!pipeline_service.RebuildActiveEditorPipeline(guard, error)) {
    pipeline_service.SavePipeline(guard);
    return false;
  }
  guard->serialized_state_needs_writeback_ = true;
  if (!pipeline_service.PersistEditorHistoryState(guard, expected, error)) {
    pipeline_service.SavePipeline(guard);
    return false;
  }
  if (writeback_after_persist) {
    guard->serialized_state_needs_writeback_ = true;
  }
  guard->dirty_ = false;
  pipeline_service.SavePipeline(guard);
  return true;
}

class EditorSessionHistoryPortTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    journal_path_ = std::filesystem::temp_directory_path() / ("session_history_" + stamp + ".wal");
    guard_        = MakeMiniGitPipelineGuard(42);
    root_graph_   = std::make_shared<alcedo::CommitGraph>(*guard_->commit_graph_);
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(
        EditorSessionPipelineMappers{{}, [guard = guard_](sl_element_id_t) { return guard; }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
  }

  void TearDown() override {
    history_.Release({42, true});
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  std::filesystem::path                      journal_path_;
  std::shared_ptr<alcedo::PipelineGuard>     guard_;
  std::shared_ptr<alcedo::CommitGraph>       root_graph_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorSessionHistoryPortTest, ActiveVersionIdentityReadReturnsOnlyTheCheckedOutRef) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  alcedo::version_ref_id_t active_version_id;

  ASSERT_TRUE(history_.ReadActiveVersionId(handle, &active_version_id, &error)) << error;

  EXPECT_EQ(active_version_id, guard_->commit_graph_->GetActiveVersionId());
}

TEST(EditorHistoryPureReducerTest, ReplaysHeadWithoutConstructingRenderExecutor) {
  auto graph = alcedo::CommitGraph::CreateEmpty(43);
  auto root_snapshot = MakeEmptyCompleteAdjustmentSnapshot();

  alcedo::PipelineEditBatch batch;
  alcedo::SetParameterChange change;
  change.target.owner_kind             = alcedo::PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = alcedo::NodeId{"grade.primary"};
  change.target.adjustment_instance_id = alcedo::AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = nlohmann::json{{"exposure_ev", 0.0}};
  change.after_value                   = nlohmann::json{{"exposure_ev", 0.75}};
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = alcedo::PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));

  const auto commit = alcedo::EditCommit::MakePipelineEdit(graph.GetRootId(), std::nullopt, batch);
  ASSERT_TRUE(graph.InsertCommit(commit));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), commit.GetCommitHash());

  auto expected = root_snapshot;
  std::string error;
  ASSERT_TRUE(ApplyHistoryCommitToSnapshot(&expected, graph, commit, true, &error)) << error;

  alcedo::EditorRenderAdjustmentSnapshot actual;
  ASSERT_TRUE(SnapshotAtHead(root_snapshot, graph, commit.GetCommitHash(), &actual, &error))
      << error;
  EXPECT_EQ(PatchValue(actual, "exposure"), PatchValue(expected, "exposure"));
  EXPECT_EQ(actual.patches.size(), kEditorSnapshotFields.size());
  EXPECT_TRUE(IsCompleteAdjustmentSnapshot(actual, &error)) << error;
}

TEST_F(EditorSessionHistoryPortTest, SettledAdjustmentCreatesOneCommitAndUndoRedoMovesHead) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto preview = WithColorGradeTarget({"exposure", R"({"exposure":0.25})", false});
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":0.75})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  ASSERT_EQ(guard_->commit_graph_->CommitCount(), 1u);
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  ASSERT_TRUE(history_.Redo(handle, &error)) << error;
  EXPECT_TRUE(guard_->working_head_commit_hash().has_value());
}

TEST_F(EditorSessionHistoryPortTest, LiveWriteProjectsTypedExposureWithoutReadingParamsJson) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto preview = WithColorGradeTarget({"exposure", R"({"exposure":0.75})", false});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;

  alcedo::EditorPanelProjection projection;
  ASSERT_TRUE(history_.ReadPanelProjection(handle, &projection, &error)) << error;
  const alcedo::EditorPanelFieldPresentation* exposure = nullptr;
  for (const auto& field : projection.fields) {
    if (field.field_key == "exposure") {
      exposure = &field;
      break;
    }
  }
  ASSERT_NE(exposure, nullptr);
  const auto* scalar = std::get_if<alcedo::EditorPanelScalarValue>(&exposure->value);
  ASSERT_NE(scalar, nullptr);
  EXPECT_FLOAT_EQ(scalar->value, 0.75f);
  EXPECT_EQ(exposure->source.adjustment_instance_id.Value(), "grade.primary.exposure");
}

TEST_F(EditorSessionHistoryPortTest, BranchingVersionHistoryAllowsSwitchingWithoutMergeCommits) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Commit settled edit on main version.
  const auto preview = WithColorGradeTarget({"exposure", R"({"exposure":0.0})", true});
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":0.5})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  const auto main_head = guard_->working_head_commit_hash();
  ASSERT_TRUE(main_head.has_value());

  // Create branch version at root.
  const auto branch_version = guard_->commit_graph_->CreateVersionRefAtRoot("Branch");
  guard_->commit_graph_->SetActiveVersionId(branch_version);

  // Commit on branch version.
  const auto preview2 = WithColorGradeTarget({"contrast", R"({"contrast":0.3})", false});
  const auto settled2 = WithColorGradeTarget({"contrast", R"({"contrast":0.3})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, preview2, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled2, &error)) << error;
  const auto branch_head = guard_->working_head_commit_hash();
  ASSERT_TRUE(branch_head.has_value());
  EXPECT_NE(*branch_head, *main_head);

  // First-parent chain for branch only contains branch commit.
  const auto branch_chain = guard_->commit_graph_->FirstParentChain(*branch_head);
  ASSERT_EQ(branch_chain.size(), 1u);
  EXPECT_EQ(branch_chain[0], *branch_head);
}

template <typename T, typename = void>
struct HasBeginLiveMerge : std::false_type {};
template <typename T>
struct HasBeginLiveMerge<T, std::void_t<decltype(&T::BeginLiveMerge)>> : std::true_type {};

TEST_F(EditorSessionHistoryPortTest, HistoryPortSurfaceHasNoMergeMethods) {
  static_assert(!HasBeginLiveMerge<alcedo::IEditorHistoryPort>::value,
                "IEditorHistoryPort must not contain BeginLiveMerge");
}

TEST_F(EditorSessionHistoryPortTest, TransferCandidateBuildFailureLeavesPublishedGraphUntouched) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto active_version = guard_->commit_graph_->GetActiveVersionId();
  const auto commit_count   = guard_->commit_graph_->CommitCount();
  alcedo::AdjustmentPasteResult paste_result;
  EXPECT_FALSE(history_.PasteLiveRootRelativeVersion(
      handle, alcedo::AdjustmentTransferPackage{}, "Pasted", &paste_result, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(paste_result.pasted);
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), active_version);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), commit_count);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
}

TEST_F(EditorSessionHistoryPortTest,
       PasteCreatesNewVersionAndLivePipelineReceivesOperatorParams) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto prior_version = guard_->commit_graph_->GetActiveVersionId();
  const auto package = MakeExposureTransferPackage(1.5);
  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(history_.PasteLiveRootRelativeVersion(handle, package, "Pasted Live", &paste_result,
                                                    &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);
  EXPECT_EQ(paste_result.prior_version_id, prior_version);
  EXPECT_NE(guard_->commit_graph_->GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), paste_result.new_version_id);
  ASSERT_TRUE(guard_->working_head_commit_hash().has_value());
  EXPECT_EQ(*guard_->working_head_commit_hash(), paste_result.new_head);

  alcedo::EditorAdjustmentOperatorState exposure_state;
  ASSERT_TRUE(alcedo::ReadEditorAdjustmentOperatorState(*guard_->pipeline_, "exposure",
                                                        &exposure_state, &error))
      << error;
  EXPECT_DOUBLE_EQ(exposure_state.params.at("exposure").get<double>(), 1.5);
  EXPECT_TRUE(exposure_state.enabled);

  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(snapshot, "exposure"), R"({"exposure":1.5})");

  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 1u);
}

TEST_F(EditorSessionHistoryPortTest,
       PasteCancelRestoresPriorActiveVersionAndPipelineParams) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.25})", &error))
      << error;
  const auto prior_version = guard_->commit_graph_->GetActiveVersionId();
  alcedo::EditorRenderAdjustmentSnapshot prior_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &prior_snapshot, &error)) << error;
  {
    std::unique_lock<std::mutex> render_lock(guard_->pipeline_->GetRenderLock());
    ASSERT_TRUE(
        alcedo::ApplyEditorAdjustmentSnapshot(*guard_->pipeline_, prior_snapshot, &error))
        << error;
  }

  const auto package = MakeExposureTransferPackage(2.0);
  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(history_.PasteLiveRootRelativeVersion(handle, package, "Pasted To Cancel",
                                                    &paste_result, &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);
  EXPECT_NE(guard_->commit_graph_->GetActiveVersionId(), prior_version);

  ASSERT_TRUE(history_.CancelLivePaste(handle, paste_result.prior_version_id,
                                       paste_result.new_version_id, &error))
      << error;
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), prior_version);
  EXPECT_EQ(guard_->commit_graph_->GetAllVersionRefs().count(paste_result.new_version_id), 0u);

  alcedo::EditorAdjustmentOperatorState restored;
  ASSERT_TRUE(
      alcedo::ReadEditorAdjustmentOperatorState(*guard_->pipeline_, "exposure", &restored, &error))
      << error;
  EXPECT_DOUBLE_EQ(restored.params.at("exposure").get<double>(), 0.25);

  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(snapshot, "exposure"), R"({"exposure":0.25})");
}

TEST_F(EditorSessionHistoryPortTest, TypedEditChangesDocumentBeforeOrWithWalAppend) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error))
      << error;
  nlohmann::json actual;
  ASSERT_TRUE(ReadEditorParameterJson(*guard_->document_, ColorGradeTargetForField("exposure"),
                                      &actual, &error))
      << error;
  EXPECT_FLOAT_EQ(actual.at("exposure_ev").get<float>(), 0.5f);

  alcedo::MiniGitJournal journal(journal_path_);
  ASSERT_TRUE(journal.Load(&error)) << error;
  EXPECT_FALSE(journal.records().empty());
}

TEST_F(EditorSessionHistoryPortTest, CommittedSnapshotMatchesDocumentValueAfterEdit) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.75})", &error))
      << error;
  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(snapshot, "exposure"), R"({"exposure":0.75})");

  nlohmann::json actual;
  ASSERT_TRUE(ReadEditorParameterJson(*guard_->document_, ColorGradeTargetForField("exposure"),
                                      &actual, &error))
      << error;
  EXPECT_FLOAT_EQ(actual.at("exposure_ev").get<float>(), 0.75f);
}

TEST_F(EditorSessionHistoryPortTest,
       PublishedPasteCaptureReopensWithExactVersionHeadChainAndAdjustment) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto package = MakeExposureTransferPackage(0.85);
  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(history_.PasteLiveRootRelativeVersion(handle, package, "Pasted Version",
                                                    &paste_result, &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);
  auto capture = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(capture)) << error;
  EXPECT_FALSE(capture->journal_records.empty());
  // Simulate successful materialize: truncate the WAL so reopen only loads the
  // captured graph (journal fold already reflected in materialization commits).
  ASSERT_TRUE(history_.DiscardMaterializedJournalThrough(
      handle, *capture->last_journal_sequence, &error))
      << error;

  const auto published_version = guard_->commit_graph_->GetActiveVersionId();
  const auto published_head    = guard_->working_head_commit_hash();
  const auto published_chain   = guard_->transaction_chain_hash();
  alcedo::EditorRenderAdjustmentSnapshot published_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &published_snapshot, &error)) << error;
  history_.Release(handle);

  // Rebuild a fresh history port from the live capture materialization.
  auto reopened_guard = MakeMiniGitPipelineGuard(42);
  reopened_guard->commit_graph_ = std::make_shared<alcedo::CommitGraph>(
      alcedo::CommitGraph::FromParts(capture->materialization.image_state,
                                     capture->materialization.version_refs,
                                     capture->materialization.commits));
  reopened_guard->root_id_                = capture->materialization.image_state.root_id;
  ASSERT_TRUE(capture->materialization.image_state.serialized_pipeline_state.has_value());
  const auto checkpoint = alcedo::DecodePipelineDocumentCheckpoint(
      *capture->materialization.image_state.serialized_pipeline_state);
  nlohmann::json captured_exposure;
  std::string    target_error;
  const auto exposure_target =
      alcedo::CompleteCurrentPanelParameterTarget(checkpoint.document, "exposure", &target_error);
  ASSERT_TRUE(exposure_target.has_value()) << target_error;
  ASSERT_TRUE(alcedo::ReadEditorParameterJson(checkpoint.document, *exposure_target,
                                              &captured_exposure, &error))
      << error;
  ASSERT_TRUE(captured_exposure.contains("exposure_ev"));
  EXPECT_NEAR(captured_exposure.at("exposure_ev").get<double>(), 0.85, 1e-5);
  reopened_guard->document_ = std::make_shared<alcedo::PipelineDocument>(
      alcedo::ClonePipelineDocument(checkpoint.document));
  reopened_guard->pipeline_->SetPipelineDocument(reopened_guard->document_, false);
  {
    std::unique_lock<std::mutex> render_lock(reopened_guard->pipeline_->GetRenderLock());
    ASSERT_TRUE(alcedo::ApplyVersionHeadToLivePipeline(
        *reopened_guard->pipeline_, *reopened_guard->commit_graph_,
        reopened_guard->commit_graph_->GetActiveVersionRef().head_commit_hash, &error))
        << error;
  }
  auto reopened_pipeline = std::make_shared<EditorSessionPipelinePort>();
  reopened_pipeline->SetServices(EditorSessionPipelineMappers{
      {}, [reopened_guard](sl_element_id_t) { return reopened_guard; }});
  EditorSessionHistoryPort reopened;
  reopened.SetServices(
      EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
  reopened.SetPipelinePort(reopened_pipeline);
  const auto reopened_handle = reopened.Acquire(42, &error);
  ASSERT_TRUE(reopened_handle.valid) << error;

  EXPECT_EQ(reopened_guard->commit_graph_->GetActiveVersionId(), published_version);
  EXPECT_EQ(reopened_guard->working_head_commit_hash(), published_head);
  EXPECT_EQ(reopened_guard->transaction_chain_hash(), published_chain);
  alcedo::EditorRenderAdjustmentSnapshot reopened_snapshot;
  ASSERT_TRUE(reopened.ReadAdjustmentSnapshot(reopened_handle, &reopened_snapshot, &error))
      << error;
  const auto published_exposure = PatchNumber(published_snapshot, "exposure");
  const auto reopened_exposure  = PatchNumber(reopened_snapshot, "exposure");
  ASSERT_TRUE(published_exposure.has_value());
  ASSERT_TRUE(reopened_exposure.has_value());
  EXPECT_NEAR(*reopened_exposure, *published_exposure, 1e-5);
  EXPECT_EQ(reopened_guard->commit_graph_->CommitCount(),
            capture->materialization.commits.size());
  reopened.Release(reopened_handle);
}

TEST_F(EditorSessionHistoryPortTest,
       InitialAdjustmentSnapshotContainsEverySupportedFieldBeforeAnyRender) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;

  const std::set<std::string> expected_fields = {
      "exposure",     "contrast",  "white",       "black",      "shadows",
      "highlights",   "curve",     "saturation",  "vibrance",   "tint",
      "hls",           "color_wheel", "lut",        "clarity",    "sharpen",
      "odt",           "film_grain", "halation",   "crop_rotate", "raw_decode",
      "lens_calib",   "color_temp"};
  std::set<std::string> actual_fields;
  for (const auto& patch : snapshot.patches) {
    actual_fields.insert(patch.field_key);
    EXPECT_FALSE(patch.params_json.empty());
  }
  EXPECT_EQ(actual_fields, expected_fields);
}

TEST_F(EditorSessionHistoryPortTest,
       UnsupportedAdjustmentFieldFailsBeforeMiniGitPublication) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto commit_count_before = guard_->commit_graph_->CommitCount();

  const auto unsupported =
      WithColorGradeTarget({"not_a_supported_adjustment", R"({})", false});
  EXPECT_FALSE(history_.CaptureAdjustmentBeforePreview(handle, unsupported, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), commit_count_before);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());

  alcedo::MiniGitJournal journal(journal_path_);
  std::string            journal_error;
  ASSERT_TRUE(journal.Load(&journal_error)) << journal_error;
  EXPECT_TRUE(journal.records().empty());
}

TEST_F(EditorSessionHistoryPortTest,
       SaveCaptureWaitsForPipelineRenderLockThenReadsLiveDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  std::atomic<bool> worker_ready{false};
  std::atomic<bool> release_worker{false};
  std::thread       worker([&] {
    std::unique_lock<std::mutex> held(guard_->pipeline_->GetRenderLock());
    worker_ready.store(true);
    while (!release_worker.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker_ready.load() && std::chrono::steady_clock::now() < ready_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(worker_ready.load());

  auto capture_future = std::async(std::launch::async, [&] {
    std::string capture_error;
    return history_.CaptureSaveCheckpoint(handle, &capture_error);
  });
  EXPECT_EQ(capture_future.wait_for(std::chrono::milliseconds(250)), std::future_status::timeout)
      << "save capture must wait for the live document render lock";

  release_worker.store(true);
  if (worker.joinable()) {
    worker.join();
  }
  EXPECT_EQ(capture_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  ASSERT_TRUE(capture_future.get());
}

TEST_F(EditorSessionHistoryPortTest,
       ProductionHistoryPortCaptureContainsElementVersionRootHeadHashStateSequenceRangeAndRecords) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto first = WithColorGradeTarget({"exposure", R"({"exposure":0.5})", true});
  const auto second = WithColorGradeTarget({"exposure", R"({"exposure":1.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;

  // Production entry surface — not the project fixture helper.
  auto capture = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(capture)) << error;
  EXPECT_EQ(capture->element_id, 42u);
  EXPECT_EQ(capture->version_id, guard_->commit_graph_->GetActiveVersionId());
  EXPECT_EQ(capture->root_id, guard_->root_id_);
  EXPECT_EQ(capture->version_id, capture->materialization.image_state.active_version_id);
  EXPECT_EQ(capture->root_id, capture->materialization.image_state.root_id);
  EXPECT_EQ(capture->journal_path, journal_path_);
  ASSERT_EQ(capture->journal_records.size(), 2u);
  ASSERT_TRUE(capture->has_journal_range());
  EXPECT_EQ(*capture->first_journal_sequence, 1u);
  EXPECT_EQ(*capture->last_journal_sequence, 2u);
  EXPECT_EQ(capture->journal_records.front().sequence, 1u);
  EXPECT_EQ(capture->journal_records.back().sequence, 2u);
  EXPECT_EQ(capture->working_head, guard_->working_head_commit_hash());
  EXPECT_EQ(capture->transaction_chain_hash, guard_->transaction_chain_hash());
  EXPECT_EQ(capture->materialization.image_state.element_id, 42u);
  ASSERT_TRUE(capture->materialization.image_state.serialized_pipeline_state.has_value());
}

TEST_F(EditorSessionHistoryPortTest,
       DiscardMaterializedJournalThroughDropsLivePrefixForSameSessionCapture) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":0.9})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  auto first = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(first)) << error;
  ASSERT_TRUE(first->has_journal_range());
  ASSERT_TRUE(history_.DiscardMaterializedJournalThrough(handle, *first->last_journal_sequence,
                                                         &error))
      << error;

  auto second = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(second)) << error;
  EXPECT_TRUE(second->journal_records.empty());
  EXPECT_FALSE(second->has_journal_range());
}

TEST_F(EditorSessionHistoryPortTest,
       DiscardReturnsToMaterializedHeadAndClearsUnmaterializedHistory) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  const auto first = WithColorGradeTarget({"exposure", R"({"exposure":0.4})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, first, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, first, &error)) << error;
  ASSERT_TRUE(history_.SyncMaterializedStateAfterCheckpoint(handle, &error)) << error;

  alcedo::EditorRenderAdjustmentSnapshot materialized_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &materialized_snapshot, &error)) << error;
  const auto materialized_head = guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash;

  const auto second = WithColorGradeTarget({"exposure", R"({"exposure":0.9})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, second, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, second, &error)) << error;
  EXPECT_TRUE(history_.HasUnmaterializedChanges(handle, &error)) << error;

  ASSERT_TRUE(history_.DiscardUnmaterializedChanges(handle, &error)) << error;
  EXPECT_FALSE(history_.HasUnmaterializedChanges(handle, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), materialized_head);
  EXPECT_FALSE(guard_->dirty_);
  EXPECT_TRUE(guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash.has_value());

  alcedo::EditorRenderAdjustmentSnapshot restored_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &restored_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(restored_snapshot, "exposure"), PatchValue(materialized_snapshot, "exposure"));
  EXPECT_TRUE(history_.CaptureSaveCheckpoint(handle, &error)->journal_records.empty());
}

TEST_F(EditorSessionHistoryPortTest,
       SyncMaterializedStateAfterCheckpointMirrorsActiveHeadIntoInMemoryState) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":0.4})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  const auto active_head = guard_->commit_graph_->GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(active_head.has_value()) << "a settled commit must advance the working head";
  // The checkpoint has not reconciled yet; the in-memory materialized head stays at root.
  EXPECT_FALSE(guard_->commit_graph_->GetImageEditState()
                   .materialized_head_commit_hash.has_value())
      << "materialized head must stay at root until a checkpoint materializes it";

  ASSERT_TRUE(history_.SyncMaterializedStateAfterCheckpoint(handle, &error)) << error;
  EXPECT_EQ(guard_->commit_graph_->GetImageEditState().materialized_head_commit_hash,
            active_head);
  EXPECT_EQ(guard_->commit_graph_->GetImageEditState().materialized_transaction_chain_hash,
            guard_->commit_graph_->ChainHashForHead(active_head));
}

TEST_F(EditorSessionHistoryPortTest, JournalAppendFailureKeepsWorkingHeadAtRoot) {
  history_.SetServices(
      EditorSessionHistoryPort::Services{[bad = journal_path_.parent_path() / "not-a-directory"](
                                             sl_element_id_t) { return bad / "image-42.wal"; }});
  {
    std::ofstream blocker(journal_path_.parent_path() / "not-a-directory", std::ios::binary);
    ASSERT_TRUE(blocker.is_open());
    blocker << "block";
  }
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":1.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  EXPECT_FALSE(history_.CommitAdjustment(handle, settled, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

TEST_F(EditorSessionHistoryPortTest, JournalAppendFailureRestoresDocumentHeadAndProjection) {
  history_.SetServices(
      EditorSessionHistoryPort::Services{[bad = journal_path_.parent_path() / "not-a-directory"](
                                             sl_element_id_t) { return bad / "image-42.wal"; }});
  {
    std::ofstream blocker(journal_path_.parent_path() / "not-a-directory", std::ios::binary);
    ASSERT_TRUE(blocker.is_open());
    blocker << "block";
  }
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto before_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->document_);
  const auto before_head = guard_->working_head_commit_hash();
  const auto settled     = WithColorGradeTarget({"exposure", R"({"exposure":1.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  EXPECT_FALSE(history_.CommitAdjustment(handle, settled, &error));
  EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard_->document_), before_hash);
  EXPECT_EQ(guard_->working_head_commit_hash(), before_head);
  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.commits.empty());
  EXPECT_FALSE(snapshot.can_undo);
  std::error_code ec;
  std::filesystem::remove(journal_path_.parent_path() / "not-a-directory", ec);
}

TEST_F(EditorSessionHistoryPortTest, ReopenReplaysJournalIntoWorkingPipeline) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":1.25})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;
  history_.Release(handle);

  auto reopened_guard           = MakeMiniGitPipelineGuard(42);
  reopened_guard->commit_graph_ = std::make_shared<alcedo::CommitGraph>(*root_graph_);
  auto reopened_pipeline        = std::make_shared<EditorSessionPipelinePort>();
  reopened_pipeline->SetServices(EditorSessionPipelineMappers{
      {}, [reopened_guard](sl_element_id_t) { return reopened_guard; }});
  EditorSessionHistoryPort reopened;
  reopened.SetServices(
      EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
  reopened.SetPipelinePort(reopened_pipeline);
  const auto reopened_handle = reopened.Acquire(42, &error);
  ASSERT_TRUE(reopened_handle.valid) << error;
  EXPECT_EQ(reopened_guard->commit_graph_->CommitCount(), 1u);
  EXPECT_TRUE(reopened_guard->working_head_commit_hash().has_value());
}

/// Phase 4A: capture returns an owned value; a second capture does not require a
/// side-map TakeSaveCapture, and the same shared_ptr identity reaches a store.
TEST_F(EditorSessionHistoryPortTest, ProductionCaptureValueReachesCheckpointStoreWithoutSideMap) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto settled = WithColorGradeTarget({"exposure", R"({"exposure":0.85})", true});
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(handle, settled, &error)) << error;
  ASSERT_TRUE(history_.CommitAdjustment(handle, settled, &error)) << error;

  auto first = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(first)) << error;
  ASSERT_TRUE(first->has_journal_range());
  const auto* first_raw = first.get();
  const auto  first_head = first->working_head;
  const auto  first_last = *first->last_journal_sequence;

  // Ownership travels as a function argument. The history port keeps no
  // element-ID side map: a second capture returns an independent value.
  auto second = history_.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(second)) << error;
  EXPECT_NE(second.get(), first_raw);
  EXPECT_EQ(second->working_head, first_head);
  EXPECT_EQ(*second->last_journal_sequence, first_last);
  // First capture remains valid after the second (no Take/rendezvous).
  EXPECT_EQ(first.get(), first_raw);
  EXPECT_EQ(first->journal_records.size(), second->journal_records.size());
}

TEST_F(EditorSessionHistoryPortTest, HistoryProjectionPublishesDisplayNameBeforeValueAndAfterValue) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.46})", &error)) << error;

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  ASSERT_EQ(snapshot.commits.size(), 2u);
  // Newest-first: the head commit is the second exposure edit.
  const auto& head = snapshot.commits.front();
  EXPECT_EQ(head.field_key, "exposure");
  EXPECT_EQ(head.position, alcedo::EditorHistoryTimelinePosition::Current);
  const auto before_exposure = ReadJsonNumber(head.before_value_json, "exposure_ev").has_value()
                                   ? ReadJsonNumber(head.before_value_json, "exposure_ev")
                                   : ReadJsonNumber(head.before_value_json, "exposure");
  const auto after_exposure  = ReadJsonNumber(head.after_value_json, "exposure_ev").has_value()
                                   ? ReadJsonNumber(head.after_value_json, "exposure_ev")
                                   : ReadJsonNumber(head.after_value_json, "exposure");
  ASSERT_TRUE(before_exposure.has_value());
  ASSERT_TRUE(after_exposure.has_value());
  EXPECT_NEAR(*before_exposure, 0.35, 1e-6);
  EXPECT_NEAR(*after_exposure, 0.46, 1e-6);

  const auto pres = PresentEditorHistoryCommit(head.field_key, head.before_value_json,
                                               head.after_value_json, head.before_enabled,
                                               head.after_enabled);
  EXPECT_EQ(pres.display_name.toStdString(), "Exposure");
  EXPECT_EQ(pres.before_text.toStdString(), "+0.35");
  EXPECT_EQ(pres.after_text.toStdString(), "+0.46");
  EXPECT_FALSE(pres.icon_key.isEmpty());
}

TEST_F(EditorSessionHistoryPortTest,
       HistoryProjectionMarksOnlyWorkingHeadCurrentAndIncludesRedoSuffix) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "contrast", R"({"contrast":12.0})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "saturation", R"({"saturation":8.0})", &error)) << error;
  // Move back one hop: head = contrast, saturation enters the redo suffix.
  ASSERT_TRUE(history_.Undo(handle, &error)) << error;

  alcedo::EditorHistorySnapshot snapshot;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &snapshot, &error)) << error;
  EXPECT_TRUE(snapshot.can_undo);
  EXPECT_TRUE(snapshot.can_redo);
  std::size_t current_count = 0;
  std::size_t future_count  = 0;
  std::size_t applied_count = 0;
  alcedo::commit_hash_t current_hash;
  for (const auto& row : snapshot.commits) {
    switch (row.position) {
      case alcedo::EditorHistoryTimelinePosition::Current:
        ++current_count;
        current_hash = row.commit_hash;
        break;
      case alcedo::EditorHistoryTimelinePosition::Future:
        ++future_count;
        break;
      case alcedo::EditorHistoryTimelinePosition::Applied:
        ++applied_count;
        break;
    }
  }
  EXPECT_EQ(current_count, 1u);
  EXPECT_EQ(future_count, 1u);
  EXPECT_EQ(applied_count, 1u);
  // The single Current row is the actual working head (contrast), not the root.
  ASSERT_TRUE(guard_->working_head_commit_hash().has_value());
  EXPECT_EQ(current_hash, *guard_->working_head_commit_hash());
}

TEST_F(EditorSessionHistoryPortTest,
       MoveHeadToAncestorThenRedoDescendantPublishesOneFinalSnapshot) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.35})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "contrast", R"({"contrast":12.0})", &error)) << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "saturation", R"({"saturation":8.0})", &error)) << error;
  // Walk the first-parent chain (root -> head) to find the ancestor and head hashes.
  const auto chain = guard_->commit_graph_->FirstParentChain(guard_->working_head_commit_hash());
  ASSERT_FALSE(chain.empty());
  const auto exposure_id   = guard_->commit_graph_->GetCommit(chain.front()).GetCommitHash();
  const auto saturation_id = guard_->commit_graph_->GetCommit(chain.back()).GetCommitHash();

  auto count_positions = [](const alcedo::EditorHistorySnapshot& s) {
    struct Counts {
      std::size_t current = 0, future = 0, applied = 0;
    } counts;
    for (const auto& row : s.commits) {
      switch (row.position) {
        case alcedo::EditorHistoryTimelinePosition::Current: ++counts.current; break;
        case alcedo::EditorHistoryTimelinePosition::Future: ++counts.future; break;
        case alcedo::EditorHistoryTimelinePosition::Applied: ++counts.applied; break;
      }
    }
    return counts;
  };

  // Backward multi-step: head -> exposure in one operation. The redo suffix
  // keeps contrast + saturation as Future rows.
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, exposure_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash().value(), exposure_id);
  alcedo::EditorHistorySnapshot backward_projection;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &backward_projection, &error)) << error;
  const auto backward_counts = count_positions(backward_projection);
  EXPECT_EQ(backward_counts.current, 1u);
  EXPECT_EQ(backward_counts.future, 2u);
  EXPECT_EQ(backward_counts.applied, 0u);
  alcedo::EditorRenderAdjustmentSnapshot backward_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &backward_snapshot, &error)) << error;
  const auto backward_exposure = PatchNumber(backward_snapshot, "exposure");
  ASSERT_TRUE(backward_exposure.has_value());
  EXPECT_NEAR(*backward_exposure, 0.35, 1e-6);

  // Forward multi-step: head -> saturation in one operation, consuming the suffix.
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, saturation_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash().value(), saturation_id);
  alcedo::EditorHistorySnapshot forward_projection;
  ASSERT_TRUE(history_.ReadHistorySnapshot(handle, &forward_projection, &error)) << error;
  const auto forward_counts = count_positions(forward_projection);
  EXPECT_EQ(forward_counts.current, 1u);
  EXPECT_EQ(forward_counts.future, 0u);
  EXPECT_EQ(forward_counts.applied, 2u);
  alcedo::EditorRenderAdjustmentSnapshot forward_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &forward_snapshot, &error)) << error;
  const auto forward_saturation = PatchNumber(forward_snapshot, "saturation");
  const auto forward_contrast   = PatchNumber(forward_snapshot, "contrast");
  ASSERT_TRUE(forward_saturation.has_value());
  ASSERT_TRUE(forward_contrast.has_value());
  EXPECT_NEAR(*forward_saturation, 4.0, 1e-6);  // Model clamps its multiplier to 4.
  EXPECT_NEAR(*forward_contrast, 12.0, 1e-6);
}

TEST(EditorHistoryCommitPresentationTest, FormatsNumericBooleanPathEnumAndCompoundAdjustments) {
  // Numeric slider.
  auto exposure = PresentEditorHistoryCommit("exposure", R"({"exposure":0.0})", R"({"exposure":0.35})",
                                             true, true);
  EXPECT_EQ(exposure.display_name.toStdString(), "Exposure");
  EXPECT_EQ(exposure.before_text.toStdString(), "0");
  EXPECT_EQ(exposure.after_text.toStdString(), "+0.35");

  // Boolean toggle (RAW highlights reconstruct).
  auto raw =
      PresentEditorHistoryCommit("raw_decode", R"({"raw":{"highlights_reconstruct":false}})",
                                 R"({"raw":{"highlights_reconstruct":true}})", true, true);
  // Path (LUT file).
  auto lut = PresentEditorHistoryCommit("lut", R"({"ocio_lmt":"C:/looks/old.cube"})",
                                        R"({"ocio_lmt":"C:/looks/teal.cube"})", true, true);
  EXPECT_EQ(lut.display_name.toStdString(), "LUT");
  EXPECT_EQ(lut.before_text.toStdString(), "old.cube");
  EXPECT_EQ(lut.after_text.toStdString(), "teal.cube");

  // Enum (ODT output target).
  auto odt = PresentEditorHistoryCommit(
      "odt", R"({"odt":{"encoding_space":"REC709","encoding_eotf":"GAMMA_2_2","peak_luminance":1000}})",
      R"({"odt":{"encoding_space":"DISPLAY_P3","encoding_eotf":"GAMMA_2_4","peak_luminance":1000}})",
      true, true);
  EXPECT_EQ(odt.display_name.toStdString(), "ODT");
  EXPECT_EQ(odt.before_text.toStdString(), "Rec.709 / Gamma 2.2");
  EXPECT_EQ(odt.after_text.toStdString(), "Display P3 / Gamma 2.4");

  // Compound (crop/rotate angle).
  auto crop = PresentEditorHistoryCommit("crop_rotate", R"({"crop_rotate":{"angle_degrees":0.0}})",
                                         R"({"crop_rotate":{"angle_degrees":12.0}})", true, true);
  EXPECT_EQ(crop.display_name.toStdString(), "Crop / Rotate");
  EXPECT_EQ(crop.after_text.toStdString(), "+12\u00b0");
}

TEST(EditorHistoryCommitPresentationTest, TypedGraphOperationsUseSavedNamesAndKeepAdjustmentRows) {
  using alcedo::EditorHistoryCommit;
  using alcedo::PipelineEditOperationKind;
  using alcedo::PresentationKeyForOperation;

  EditorHistoryCommit add;
  add.presentation_key  = PresentationKeyForOperation(PipelineEditOperationKind::AddColorGrade);
  add.node_display_name = "Color Grade 2";
  add.after_value_json  = R"({"display_name":"Color Grade 2"})";
  const auto add_pres   = PresentEditorHistoryCommit(add);
  EXPECT_EQ(add_pres.display_name.toStdString(), "Add Color Grade");
  EXPECT_EQ(add_pres.after_text.toStdString(), "Color Grade 2");
  EXPECT_EQ(add_pres.delta_text.toStdString(), "Color Grade 2");

  EditorHistoryCommit rename;
  rename.presentation_key  = PresentationKeyForOperation(PipelineEditOperationKind::RenameColorGrade);
  rename.node_display_name = "Sky";
  rename.before_value_json = R"("Color Grade 1")";
  rename.after_value_json  = R"("Sky")";
  const auto rename_pres   = PresentEditorHistoryCommit(rename);
  EXPECT_EQ(rename_pres.display_name.toStdString(), "Rename Color Grade");
  EXPECT_EQ(rename_pres.before_text.toStdString(), "Color Grade 1");
  EXPECT_EQ(rename_pres.after_text.toStdString(), "Sky");
  EXPECT_EQ(rename_pres.delta_text.toStdString(), "Color Grade 1 \u2192 Sky");

  EditorHistoryCommit remove;
  remove.presentation_key  = PresentationKeyForOperation(PipelineEditOperationKind::RemoveColorGrade);
  remove.node_display_name = "Color Grade 2";
  remove.after_value_json  = R"({"display_name":"Color Grade 2"})";
  const auto remove_pres   = PresentEditorHistoryCommit(remove);
  EXPECT_EQ(remove_pres.display_name.toStdString(), "Delete Color Grade");
  EXPECT_EQ(remove_pres.delta_text.toStdString(), "Color Grade 2");

  EditorHistoryCommit exposure;
  exposure.field_key         = "exposure";
  exposure.presentation_key  = PresentationKeyForOperation(PipelineEditOperationKind::SetParameter);
  exposure.before_value_json = R"({"exposure":0.0})";
  exposure.after_value_json  = R"({"exposure":0.35})";
  exposure.before_enabled    = true;
  exposure.after_enabled     = true;
  const auto exposure_pres   = PresentEditorHistoryCommit(exposure);
  EXPECT_EQ(exposure_pres.display_name.toStdString(), "Exposure");
  EXPECT_EQ(exposure_pres.after_text.toStdString(), "+0.35");
}

// ---------------------------------------------------------------------------
// History without same-session document targets must be rejected before WAL publication.
// Typed merge and cross-session replay are outside the document parameter edit boundary.
// ---------------------------------------------------------------------------

TEST_F(EditorSessionHistoryPortTest, UnmappedHeadMovePreservesHeadPipelineSnapshotAndJournal) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Commit one settled exposure edit so the working head and committed snapshot
  // are well-defined before the failing move.
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error))
      << error;
  const auto c1 = *guard_->working_head_commit_hash();
  ASSERT_TRUE(guard_->commit_graph_->FindCommit(c1) != nullptr);

  // Insert a typed batch commit whose target does not map to any
  // document adjustment instance.
  alcedo::PipelineEditBatch bad_batch;
  alcedo::SetParameterChange bad_change;
  bad_change.target.owner_kind             = alcedo::PipelineParameterOwnerKind::ColorGrade;
  bad_change.target.node_id                = alcedo::NodeId{"grade.primary"};
  bad_change.target.adjustment_instance_id = alcedo::AdjustmentInstanceId{"grade.primary.bogus"};
  bad_change.target.field_key              = "bogus_field";
  bad_change.before_value                  = nlohmann::json{{"bogus_param", 0.0}};
  bad_change.after_value                   = nlohmann::json{{"bogus_param", 1.0}};
  bad_change.before_enabled                = true;
  bad_change.after_enabled                 = true;
  bad_batch.operation_kind                 = alcedo::PipelineEditOperationKind::SetParameter;
  bad_batch.presentation_key               = "history.operation.set_parameter";
  bad_batch.changes.push_back(std::move(bad_change));
  auto bad_commit = alcedo::EditCommit::MakePipelineEdit(guard_->commit_graph_->GetRootId(), c1,
                                                         std::move(bad_batch));
  const auto bad_head = bad_commit.GetCommitHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(std::move(bad_commit)));

  guard_->commit_graph_->MoveWorkingHead(guard_->commit_graph_->GetActiveVersionId(), bad_head);
  const auto                     before_document = guard_->document_->ToJson();
  const auto                     before_pipeline = guard_->pipeline_->ExportPipelineParams();
  const auto                     before_head     = guard_->working_head_commit_hash();
  EditorRenderAdjustmentSnapshot before_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &before_snapshot, &error)) << error;
  MiniGitJournal before_journal(journal_path_);
  ASSERT_TRUE(before_journal.Load(&error)) << error;
  const auto records = before_journal.records().size();
  EXPECT_FALSE(history_.MoveHeadToCommit(handle, c1, &error));
  EXPECT_NE(error.find("SetParameter"), std::string::npos);
  EXPECT_EQ(guard_->working_head_commit_hash(), before_head);
  EXPECT_EQ(guard_->document_->ToJson(), before_document);
  EXPECT_EQ(guard_->pipeline_->ExportPipelineParams(), before_pipeline);
  EditorRenderAdjustmentSnapshot after_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &after_snapshot, &error)) << error;
  EXPECT_TRUE(alcedo::test::SameSnapshotProjection(after_snapshot, before_snapshot));
  MiniGitJournal after_journal(journal_path_);
  ASSERT_TRUE(after_journal.Load(&error)) << error;
  EXPECT_EQ(after_journal.records().size(), records);
}

TEST_F(EditorSessionHistoryPortTest, FirstParentChainNavigationPreservesStateAcrossMultipleCommits) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;

  // Commit C1 (exposure = 0.5)
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error))
      << error;
  const auto c1 = *guard_->working_head_commit_hash();

  // Commit C2 (exposure = 0.9)
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.9})", &error))
      << error;
  const auto c2 = *guard_->working_head_commit_hash();
  EXPECT_NE(c1, c2);

  // Navigate back to C1
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, c1, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), c1);

  EditorRenderAdjustmentSnapshot c1_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &c1_snapshot, &error)) << error;
  const auto c1_exposure = PatchNumber(c1_snapshot, "exposure");
  ASSERT_TRUE(c1_exposure.has_value());
  EXPECT_NEAR(*c1_exposure, 0.5, 1e-6);

  // Navigate forward to C2
  ASSERT_TRUE(history_.MoveHeadToCommit(handle, c2, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), c2);

  EditorRenderAdjustmentSnapshot c2_snapshot;
  ASSERT_TRUE(history_.ReadAdjustmentSnapshot(handle, &c2_snapshot, &error)) << error;
  const auto c2_exposure = PatchNumber(c2_snapshot, "exposure");
  ASSERT_TRUE(c2_exposure.has_value());
  EXPECT_NEAR(*c2_exposure, 0.9, 1e-6);
}

TEST(EditorSessionHistoryPortPersistTest,
     CheckoutDefaultAfterPastePersistsWithoutLiveIdentityError) {
  alcedo::TimeProvider::Refresh();
  RegisterAllOperators();

  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("paste_checkout_default_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("paste_checkout_default_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("paste_checkout_default_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 821;
  alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
  auto pipeline_service =
      std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());

  auto guard = pipeline_service->LoadEditorPipeline(element_id);
  ASSERT_NE(guard, nullptr);
  ASSERT_NE(guard->commit_graph_, nullptr);
  const auto default_version_id = guard->commit_graph_->GetActiveVersionId();
  EXPECT_EQ(guard->commit_graph_->GetVersionRef(default_version_id).display_name, "Default");

  auto pipeline = std::make_shared<EditorSessionPipelinePort>();
  pipeline->SetServices(EditorSessionPipelineMappers{
      [pipeline_service]() { return pipeline_service; },
      [guard](sl_element_id_t) { return guard; }});

  EditorSessionHistoryPort history;
  history.SetServices(
      EditorSessionHistoryPort::Services{[journal_path](sl_element_id_t) { return journal_path; }});
  history.SetPipelinePort(pipeline);

  std::string error;
  const auto handle = history.Acquire(element_id, &error);
  ASSERT_TRUE(handle.valid) << error;

  alcedo::EditorRenderAdjustmentSnapshot baseline_snapshot;
  ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &baseline_snapshot, &error)) << error;

  const auto package = MakeExposureTransferPackage(0.85);
  alcedo::AdjustmentPasteResult paste_result;
  ASSERT_TRUE(history.PasteLiveRootRelativeVersion(handle, package, "Pasted Adjustments",
                                                   &paste_result, &error))
      << error;
  ASSERT_TRUE(paste_result.pasted);

  auto capture = history.CaptureSaveCheckpoint(handle, &error);
  ASSERT_TRUE(static_cast<bool>(capture)) << error;
  {
    auto db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto db_lock  = db_guard.Lock();
    alcedo::CommitGraphStore graph_service(db_guard.conn_);
    graph_service.Materialize(capture->materialization);
  }
  ASSERT_TRUE(history.DiscardMaterializedJournalThrough(handle, *capture->last_journal_sequence,
                                                        &error))
      << error;
  ASSERT_TRUE(history.SyncMaterializedStateAfterCheckpoint(handle, &error)) << error;

  EXPECT_NE(guard->commit_graph_->GetActiveVersionId(), default_version_id);
  EXPECT_EQ(guard->commit_graph_->GetActiveVersionRef().display_name, "Pasted Adjustments");
  ASSERT_TRUE(guard->working_head_commit_hash().has_value());

  alcedo::EditorRenderAdjustmentSnapshot pasted_snapshot;
  ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &pasted_snapshot, &error)) << error;
  const auto pasted_ev = PatchNumber(pasted_snapshot, "exposure");
  ASSERT_TRUE(pasted_ev.has_value());
  EXPECT_NEAR(*pasted_ev, 0.85, 1e-5);

  // Regression: checkout Default after Paste must persist without
  // "live history identity changed before editor history persistence".
  ASSERT_TRUE(history.CheckoutVersion(handle, default_version_id, &error)) << error;
  EXPECT_EQ(guard->commit_graph_->GetActiveVersionId(), default_version_id);
  EXPECT_FALSE(guard->working_head_commit_hash().has_value());

  alcedo::EditorRenderAdjustmentSnapshot default_snapshot;
  ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &default_snapshot, &error)) << error;
  EXPECT_EQ(PatchValue(default_snapshot, "exposure"), PatchValue(baseline_snapshot, "exposure"));

  history.Release(handle);
  pipeline_service->SavePipeline(guard);

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortProjectTest,
     PasteCrashRecoveryReplaysWalOntoLogicalHead) {
  RegisterAllOperators();
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("paste_wal_recovery_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("paste_wal_recovery_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("paste_wal_recovery_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 822;
  alcedo::version_ref_id_t pasted_version{};
  alcedo::commit_hash_t pasted_head{};
  alcedo::transaction_chain_hash_t pasted_chain{};

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    alcedo::AdjustmentPasteResult paste_result;
    ASSERT_TRUE(history.PasteLiveRootRelativeVersion(
        handle, MakeExposureTransferPackage(1.15), "Pasted WAL", &paste_result, &error))
        << error;
    ASSERT_TRUE(paste_result.pasted);
    pasted_version = paste_result.new_version_id;
    pasted_head = paste_result.new_head;
    pasted_chain = guard->transaction_chain_hash();
    // Crash after WAL append + empty Version persist, before ordinary journal
    // materialize and before SavePipeline serialized writeback (which would
    // otherwise advance materialized head while the WAL still describes the
    // paste edit prefix).
    history.Release(handle);
    project.SaveProject(meta_path);
  }

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    EXPECT_EQ(guard->commit_graph_->GetActiveVersionId(), pasted_version);
    EXPECT_EQ(guard->working_head_commit_hash(), pasted_head);
    EXPECT_EQ(guard->transaction_chain_hash(), pasted_chain);

    alcedo::EditorRenderAdjustmentSnapshot recovered;
    ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &recovered, &error)) << error;
    const auto recovered_exposure = PatchNumber(recovered, "exposure");
    ASSERT_TRUE(recovered_exposure.has_value());
    EXPECT_NEAR(*recovered_exposure, 1.15, 1e-5);

    {
      std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
      ASSERT_TRUE(alcedo::ApplyEditorAdjustmentSnapshot(*guard->pipeline_, recovered, &error))
          << error;
    }
    alcedo::EditorAdjustmentOperatorState exposure_state;
    ASSERT_TRUE(alcedo::ReadEditorAdjustmentOperatorState(*guard->pipeline_, "exposure",
                                                          &exposure_state, &error))
        << error;
    EXPECT_NEAR(exposure_state.params.at("exposure").get<double>(), 1.15, 1e-5);

    history.Release(handle);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortProjectTest,
     LoadAttachesCompatibleWalThenComparesCheckpoint) {
  RegisterAllOperators();
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("wal_checkpoint_compare_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("wal_checkpoint_compare_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("wal_checkpoint_compare_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 823;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    alcedo::AdjustmentPasteResult paste_result;
    ASSERT_TRUE(history.PasteLiveRootRelativeVersion(
        handle, MakeExposureTransferPackage(1.45), "WAL Checkpoint Compare", &paste_result, &error))
        << error;
    ASSERT_TRUE(paste_result.pasted);
    // Crash with dirty WAL: checkpoint still describes the pre-paste head.
    history.Release(handle);
    project.SaveProject(meta_path);
  }

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    alcedo::EditorRenderAdjustmentSnapshot recovered;
    ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &recovered, &error)) << error;
    const auto recovered_exposure = PatchNumber(recovered, "exposure");
    ASSERT_TRUE(recovered_exposure.has_value());
    EXPECT_NEAR(*recovered_exposure, 1.45, 1e-5);

    // EnsureWorkingState attaches the WAL then syncs the live pipeline when the
    // checkpoint identity no longer matches the logical head.
    alcedo::EditorAdjustmentOperatorState exposure_state;
    ASSERT_TRUE(alcedo::ReadEditorAdjustmentOperatorState(*guard->pipeline_, "exposure",
                                                          &exposure_state, &error))
        << error;
    EXPECT_NEAR(exposure_state.params.at("exposure").get<double>(), 1.45, 1e-5);

    history.Release(handle);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

/// Incompatible WAL must fail closed: EnsureWorkingState / Acquire reject load
/// rather than silently discarding records or applying a broken prefix.
TEST(EditorSessionHistoryPortProjectTest, LoadRejectsOrQuarantinesIncompatibleWal) {
  RegisterAllOperators();
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("wal_incompatible_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("wal_incompatible_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("wal_incompatible_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 824;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    project.SaveProject(meta_path);
  }

  // Inject a journal edit whose parent is unknown — cannot extend logical head.
  {
    alcedo::MiniGitJournal journal(journal_path);
    alcedo::PipelineEditBatch batch;
    alcedo::SetParameterChange change;
    change.target.owner_kind             = alcedo::PipelineParameterOwnerKind::ColorGrade;
    change.target.node_id                = alcedo::NodeId{"grade.primary"};
    change.target.adjustment_instance_id = alcedo::AdjustmentInstanceId{"grade.primary.exposure"};
    change.target.field_key              = "exposure";
    change.before_value                  = nlohmann::json{{"exposure_ev", 0.0}};
    change.after_value                   = nlohmann::json{{"exposure_ev", 9.9}};
    change.before_enabled                = false;
    change.after_enabled                 = true;
    batch.operation_kind                 = alcedo::PipelineEditOperationKind::SetParameter;
    batch.presentation_key               = "history.operation.set_parameter";
    batch.changes.push_back(std::move(change));
    const auto orphan_parent =
        alcedo::commit_hash_t{0xDEADBEEFDEADBEEFULL, 0xCAFEBABECAFEBABEULL};
    auto edit = alcedo::EditCommit::MakePipelineEdit(alcedo::root_id_t{}, orphan_parent, std::move(batch));
    alcedo::MiniGitJournalRecord record;
    record.kind                   = alcedo::MiniGitJournalRecordKind::kEditCommit;
    record.expected_source_head   = orphan_parent;
    record.edit_commit            = std::move(edit);
    record.target_head            = record.edit_commit->GetCommitHash();
    std::string journal_error;
    ASSERT_TRUE(journal.Append(record, &journal_error)) << journal_error;
  }

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto handle = history.Acquire(element_id, &error);
    EXPECT_FALSE(handle.valid)
        << "incompatible WAL must fail closed; Acquire should not succeed silently";
    EXPECT_FALSE(error.empty()) << "failure must surface an explicit recovery error";
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortPersistTest,
     LibraryPasteOfLutRestoresLutFieldInAdjustmentSnapshotOnEditorReopen) {
  alcedo::TimeProvider::Refresh();
  RegisterAllOperators();

  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_snapshot_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_snapshot_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_snapshot_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 831;
  const std::string         lut_path   = "D:/luts/teal_orange.cube";

  alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
  {
    alcedo::PipelineMgmtService pipeline_service(project.GetStorage());
    std::string                 error;
    ASSERT_TRUE(LibraryPasteThenRelease(pipeline_service, element_id, MakeLutTransferPackage(lut_path),
                                        true, &error))
        << error;
  }

  {
    // New service instance forces LoadEditorPipeline to read persisted state
    // instead of the first service's cache.
    auto pipeline_service = std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard            = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    ASSERT_NE(guard->pipeline_, nullptr);
    EXPECT_EQ(LiveLutPath(guard), lut_path);

    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    alcedo::EditorRenderAdjustmentSnapshot snapshot;
    ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;
    EXPECT_EQ(LutPathFromSnapshot(snapshot), lut_path)
        << "editor reopen after library Paste must publish the pasted LUT path so LUTPanel "
           "can highlight the catalog row instead of None";

    history.Release(handle);
    pipeline_service->SavePipeline(guard);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortPersistTest,
     LibraryPasteWithoutSerializedCheckpointStillRestoresLutSnapshotFromLivePipeline) {
  alcedo::TimeProvider::Refresh();
  RegisterAllOperators();

  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_live_" + stamp + ".db");
  const auto meta_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_live_" + stamp + ".json");
  const auto journal_path =
      std::filesystem::temp_directory_path() / ("library_paste_lut_live_" + stamp + ".wal");
  std::error_code ec;
  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);

  constexpr sl_element_id_t element_id = 832;
  const std::string         lut_path   = "D:/luts/film_print.cube";

  alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
  {
    alcedo::PipelineMgmtService pipeline_service(project.GetStorage());
    std::string                 error;
    // Old library-paste path: Persist clears the checkpoint and leaves writeback
    // false, so SavePipeline does not store a document checkpoint.
    ASSERT_TRUE(LibraryPasteThenRelease(pipeline_service, element_id, MakeLutTransferPackage(lut_path),
                                        false, &error))
        << error;
  }

  {
    auto pipeline_service = std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard            = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    ASSERT_NE(guard->pipeline_, nullptr);
    EXPECT_EQ(LiveLutPath(guard), lut_path);
    EXPECT_TRUE(guard->serialized_state_needs_writeback_)
        << "missing checkpoint must rebuild from history and mark writeback";

    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; },
        [guard](sl_element_id_t) { return guard; }});

    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);

    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;

    alcedo::EditorRenderAdjustmentSnapshot snapshot;
    ASSERT_TRUE(history.ReadAdjustmentSnapshot(handle, &snapshot, &error)) << error;
    EXPECT_EQ(LutPathFromSnapshot(snapshot), lut_path)
        << "when the serialized checkpoint is missing, the editor snapshot must still "
           "come from the rebuilt live pipeline so LUTPanel does not highlight None";

    history.Release(handle);
    pipeline_service->SavePipeline(guard);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

}  // namespace
}  // namespace alcedo::ui
