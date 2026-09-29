//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_render_intent.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/mask/mask_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "type/hash_type.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

namespace alcedo::ui {
namespace {

auto NodeHistoryPath(std::string_view name, std::string_view ext) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/node_history"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + std::string{ext});
}

using alcedo::test::EditorHistoryGraph;
using alcedo::test::EditorWorkingChain;
using alcedo::test::EditorWorkingHead;
using alcedo::test::EditorWorkingPreview;

auto ColorGradeTarget(const std::string& field) -> alcedo::EditorParameterTarget {
  return alcedo::test::ColorGradeFieldTarget(field);
}

auto CommitSettled(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                   const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview =
      alcedo::test::PatchFromJson(field, after_json, false);
  alcedo::EditorAdjustmentPatch settled =
      alcedo::test::PatchFromJson(field, after_json, true);
  preview = alcedo::test::WithColorGradeTarget(std::move(preview));
  settled = alcedo::test::WithColorGradeTarget(std::move(settled));
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto CommitPanelField(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                      const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview =
      alcedo::test::PatchFromJson(field, after_json, false);
  alcedo::EditorAdjustmentPatch settled =
      alcedo::test::PatchFromJson(field, after_json, true);
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto DocumentHash(const alcedo::PipelineDocument& document) -> std::string {
  return alcedo::CanonicalPipelineDocumentJson(document);
}

/// Stored checkpoint of @p element_id, or nullopt when storage holds none.
auto StoredCheckpoint(alcedo::ProjectService& project, sl_element_id_t element_id)
    -> std::optional<alcedo::PipelineDocumentCheckpoint> {
  auto                     db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto                     db_lock  = db_guard.Lock();
  alcedo::CommitGraphStore graph_service(db_guard.conn_);
  const auto               graph = graph_service.LoadGraph(element_id);
  if (!graph.has_value() || !graph->GetImageEditState().serialized_pipeline_state.has_value()) {
    return std::nullopt;
  }
  return alcedo::DecodePipelineDocumentCheckpoint(
      *graph->GetImageEditState().serialized_pipeline_state);
}

auto DocumentExposureEv(const alcedo::PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(alcedo::ReadEditorParameterJson(document, ColorGradeTarget("exposure"), &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

auto CommitCapturedAddColorGrade(EditorSessionHistoryPort&               history,
                                 const alcedo::EditorHistoryGuardHandle& handle,
                                 const alcedo::PipelineDocument&         document,
                                 const alcedo::NodeId& before_node_id, const alcedo::NodeId& new_id,
                                 std::string* error) -> bool {
  try {
    auto change = alcedo::CaptureAddColorGradeChange(document, before_node_id, new_id);
    return history.CommitPipelineEditBatch(handle, alcedo::MakeAddColorGradeBatch(std::move(change)),
                                           error);
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

class EditorVersionCheckoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = NodeHistoryPath("version_checkout", ".wal");
    // The root carries the working-space camera profile, as every stored root does, so a
    // checkout of a root Version reproduces the opened document.
    lease_ =
        alcedo::test::MakeInMemoryEditorLease(42, alcedo::test::WorkingSpaceBoundDefaultDocument());
    pipeline_     = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{{}, [this](sl_element_id_t) {
                                                          ++lease_acquire_count_;
                                                          return alcedo::test::CopyEditorLease(
                                                              lease_);
                                                        }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
  }

  void TearDown() override {
    history_.Release({42, true});
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  /// Release image 42 and store its history and working document in lease_, so a test can
  /// change the history the next Acquire takes. The journal is removed: lease_ already holds
  /// its commits.
  void ReleaseIntoLease(const alcedo::EditorHistoryGuardHandle& handle) {
    lease_.graph_    = *Graph();
    lease_.document_ = std::make_shared<alcedo::PipelineDocument>(
        alcedo::ClonePipelineDocument(Working()->Document()));
    history_.Release(handle);
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  auto Graph() -> std::shared_ptr<const alcedo::CommitGraph> {
    return EditorHistoryGraph(history_, 42);
  }
  auto Head() -> alcedo::head_commit_hash_t { return EditorWorkingHead(history_, 42); }
  auto Working() -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
    return EditorWorkingPreview(*pipeline_, 42);
  }
  auto WorkingHash() -> std::string { return DocumentHash(Working()->Document()); }

  std::filesystem::path                      journal_path_;
  /// History and documents that the next Acquire of image 42 takes (as a copy).
  alcedo::EditorHistoryLease                 lease_;
  int                                        lease_acquire_count_ = 0;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorVersionCheckoutTest, RootVersionAlwaysRebuildsExactImmutableDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto root_hash = alcedo::CanonicalPipelineDocumentJson(lease_.root_->document);
  ASSERT_TRUE(CommitPanelField(history_, handle, "crop_rotate", R"({"rotation_degrees":12.0})",
                               &error))
      << error;
  ASSERT_TRUE(CommitPanelField(history_, handle, "color_temp",
                               R"({"custom_cct":7200.0,"wb_mode":"custom"})", &error))
      << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":2.25})", &error)) << error;
  ASSERT_TRUE(CommitPanelField(history_, handle, "clarity", R"({"clarity":18.0})", &error))
      << error;
  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, Working()->Document(),
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.look"},
                                          &error))
      << error;
  ASSERT_TRUE(history_.AddMask(handle, alcedo::NodeId{"grade.look"},
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}),
                               0, &error))
      << error;
  EXPECT_NE(WorkingHash(), root_hash);
  EXPECT_NE(Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);

  alcedo::version_ref_id_t root_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Root", &root_version, &error))
      << error;
  EXPECT_EQ(Graph()->GetActiveVersionId(), root_version);
  EXPECT_FALSE(Head().has_value());
  EXPECT_EQ(WorkingHash(), root_hash);
  EXPECT_EQ(Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  EXPECT_FLOAT_EQ(DocumentExposureEv(Working()->Document()),
                  DocumentExposureEv(lease_.root_->document));
  ASSERT_TRUE(history_.LastPublishedRenderReason().has_value());
  EXPECT_EQ(*history_.LastPublishedRenderReason(), alcedo::EditorRenderReason::VersionDocumentChanged);
}

TEST_F(EditorVersionCheckoutTest, BranchVersionSharesCommitsAndKeepsIndependentHead) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.75})", &error)) << error;
  const auto shared_head = Head();
  ASSERT_TRUE(shared_head.has_value());
  EXPECT_EQ(Graph()->CommitCount(), 1u);

  alcedo::version_ref_id_t branch_id{};
  ASSERT_TRUE(history_.BranchFromCommitAndCheckout(handle, *shared_head, "Look B", &branch_id,
                                                   &error))
      << error;
  EXPECT_EQ(Graph()->CommitCount(), 1u);
  EXPECT_EQ(Graph()->GetAllVersionRefs().size(), 2u);
  EXPECT_EQ(Head(), shared_head);

  ASSERT_TRUE(CommitCapturedAddColorGrade(history_, handle, Working()->Document(),
                                          alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.look"},
                                          &error))
      << error;
  ASSERT_TRUE(history_.AddMask(handle, alcedo::NodeId{"grade.look"},
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}),
                               0, &error))
      << error;
  const auto branch_head = Head();
  ASSERT_TRUE(branch_head.has_value());
  EXPECT_NE(*branch_head, *shared_head);
  EXPECT_EQ(Graph()->CommitCount(), 3u);
  EXPECT_NE(Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  const auto* branch_grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}));
  ASSERT_NE(branch_grade, nullptr);
  EXPECT_EQ(branch_grade->DisplayName(), "Color Grade 2");
  EXPECT_EQ(Working()->Document().NextColorGradeNameNumber(), 3u);

  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(Head(), shared_head);
  EXPECT_EQ(Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  EXPECT_FLOAT_EQ(DocumentExposureEv(Working()->Document()), 0.75f);
  ASSERT_NE(Working()->Document().PrimaryGrade(), nullptr);
  EXPECT_EQ(Working()->Document().PrimaryGrade()->DisplayName(), "Color Grade 1");
  EXPECT_EQ(Working()->Document().NextColorGradeNameNumber(), 2u);
  const auto default_projection =
      alcedo::EditorNodeGraphProjection::Build(Working()->Document(), 8);
  ASSERT_EQ(default_projection.nodes.size(), 3u);
  EXPECT_EQ(default_projection.nodes[1].node_id, alcedo::NodeId{"grade.primary"});
  EXPECT_EQ(default_projection.nodes[1].display_name, "Color Grade 1");

  ASSERT_TRUE(history_.CheckoutVersion(handle, branch_id, &error)) << error;
  EXPECT_EQ(Head(), branch_head);
  const auto* checked_out_grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      Working()->Document().Graph().FindNode(alcedo::NodeId{"grade.look"}));
  ASSERT_NE(checked_out_grade, nullptr);
  EXPECT_EQ(checked_out_grade->DisplayName(), "Color Grade 2");
  EXPECT_EQ(Working()->Document().NextColorGradeNameNumber(), 3u);
  const auto branch_projection = alcedo::EditorNodeGraphProjection::Build(Working()->Document(), 8);
  ASSERT_EQ(branch_projection.nodes.size(), 4u);
  EXPECT_EQ(branch_projection.nodes[2].node_id, alcedo::NodeId{"grade.look"});
  EXPECT_EQ(branch_projection.nodes[2].display_name, "Color Grade 2");
}

// A Version checkout replaces the working document under the lease the editor already holds: it
// never takes the lease again, and each replacement starts a new document lineage.
TEST_F(EditorVersionCheckoutTest, VersionCheckoutReplacesTheDocumentWithoutRetakingTheLease) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":2.0})", &error)) << error;
  const auto               edited_hash    = WorkingHash();
  const auto               edited_lineage = Working()->Lineage();

  alcedo::version_ref_id_t root_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Clean", &root_version, &error))
      << error;
  const auto root_hash    = WorkingHash();
  const auto root_lineage = Working()->Lineage();
  EXPECT_NE(root_hash, edited_hash);
  EXPECT_NE(root_lineage, edited_lineage);

  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(WorkingHash(), edited_hash);
  EXPECT_NE(Working()->Lineage(), root_lineage);

  ASSERT_TRUE(history_.CheckoutVersion(handle, root_version, &error)) << error;
  EXPECT_EQ(WorkingHash(), root_hash);
  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(WorkingHash(), edited_hash);
  EXPECT_EQ(lease_acquire_count_, 1);
}

TEST_F(EditorVersionCheckoutTest, FailedCheckoutRestoresPriorVersionAndDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error)) << error;
  const auto prior_head = Head();
  ASSERT_TRUE(prior_head.has_value());
  ReleaseIntoLease(handle);

  // CreateVersionRefAtHead refuses a missing hash. Point a real Version at a
  // missing first-parent so checkout fails closed without throwing.
  const auto missing_id = lease_.graph_.CreateVersionRefAtHead("MissingHead", prior_head);
  lease_.graph_.GetVersionRef(missing_id).head_commit_hash = alcedo::Hash128{0x11, 0x22};

  auto missing_target = alcedo::test::ColorGradeFieldTarget("exposure");
  missing_target.node_id = alcedo::NodeId{"grade.does_not_exist"};
  const auto bad_batch   = alcedo::MakeSetParameterBatch(
      missing_target, nlohmann::json{{"exposure_ev", 0.0}}, nlohmann::json{{"exposure_ev", 3.0}},
      true, true, "missing");
  auto bad_commit =
      alcedo::EditCommit::MakePipelineEdit(lease_.graph_.GetRootId(), std::nullopt, bad_batch);
  const auto bad_hash = bad_commit.GetCommitHash();
  ASSERT_TRUE(lease_.graph_.InsertCommit(std::move(bad_commit)));
  const auto bad_id   = lease_.graph_.CreateVersionRefAtHead("InvalidBatch", bad_hash);

  // Reopen on the history that holds both unreplayable Versions.
  const auto reopened = history_.Acquire(42, &error);
  ASSERT_TRUE(reopened.valid) << error;
  ASSERT_EQ(Head(), prior_head);
  const auto prior_hash    = WorkingHash();
  const auto prior_reason  = history_.LastPublishedRenderReason();
  const auto prior_lineage = Working()->Lineage();

  EXPECT_FALSE(history_.CheckoutVersion(reopened, missing_id, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(Graph()->GetActiveVersionId(), default_id);
  EXPECT_EQ(Head(), prior_head);
  EXPECT_EQ(WorkingHash(), prior_hash);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(Working()->Lineage(), prior_lineage);

  error.clear();
  EXPECT_FALSE(history_.CheckoutVersion(reopened, bad_id, &error));
  EXPECT_NE(error.find("grade.does_not_exist"), std::string::npos) << error;
  EXPECT_EQ(Graph()->GetActiveVersionId(), default_id);
  EXPECT_EQ(WorkingHash(), prior_hash);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  // Build-then-swap: the replayed document was never bound, so the prior one stays live.
  EXPECT_EQ(Working()->Lineage(), prior_lineage) << "the executor keeps the prior binding";
}

TEST_F(EditorVersionCheckoutTest, VersionRefRestoreFailureKeepsPriorDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = Graph()->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error)) << error;
  ReleaseIntoLease(handle);

  auto       missing_target = alcedo::test::ColorGradeFieldTarget("exposure");
  missing_target.node_id    = alcedo::NodeId{"grade.does_not_exist"};
  auto bad_commit           = alcedo::EditCommit::MakePipelineEdit(
      lease_.graph_.GetRootId(), std::nullopt,
      alcedo::MakeSetParameterBatch(missing_target, nlohmann::json{{"exposure_ev", 0.0}},
                                              nlohmann::json{{"exposure_ev", 3.0}}, true, true, "missing"));
  const auto bad_hash = bad_commit.GetCommitHash();
  ASSERT_TRUE(lease_.graph_.InsertCommit(std::move(bad_commit)));

  // Reopen on the history that holds the unreplayable commit.
  const auto reopened = history_.Acquire(42, &error);
  ASSERT_TRUE(reopened.valid) << error;
  const auto               prior_hash    = WorkingHash();
  const auto               prior_head    = Head();
  const auto               prior_refs    = Graph()->GetAllVersionRefs().size();
  const auto               prior_reason  = history_.LastPublishedRenderReason();
  const auto               prior_lineage = Working()->Lineage();

  // A branch from the unreplayable commit selects the new Version, then replay fails.
  alcedo::version_ref_id_t branch_id{};
  EXPECT_FALSE(history_.BranchFromCommitAndCheckout(reopened, bad_hash, "Broken branch", &branch_id,
                                                    &error));
  EXPECT_NE(error.find("grade.does_not_exist"), std::string::npos) << error;
  EXPECT_EQ(Graph()->GetActiveVersionId(), default_id);
  EXPECT_EQ(Graph()->GetAllVersionRefs().size(), prior_refs);
  EXPECT_EQ(Head(), prior_head);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
  EXPECT_EQ(Working()->Lineage(), prior_lineage) << "the executor keeps the prior binding";
  EXPECT_EQ(WorkingHash(), prior_hash);
  EXPECT_FLOAT_EQ(DocumentExposureEv(Working()->Document()), 0.5f);
}

TEST(EditorSessionHistoryPortProjectTest, RecoveryAppliesCommittedTypedSuffixExactlyOnce) {
  const auto db_path     = NodeHistoryPath("typed_wal_recovery", ".db");
  const auto meta_path   = NodeHistoryPath("typed_wal_recovery", ".json");
  const auto journal_path = NodeHistoryPath("typed_wal_recovery", ".wal");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 831;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    pipeline_service->InitializeImageRoot(element_id, alcedo::CreateDefaultPipelineDocument(),
                                          nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(
        EditorSessionPipelineMappers{[pipeline_service]() { return pipeline_service; }, {}});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    ASSERT_TRUE(CommitSettled(history, handle, "exposure", R"({"exposure":1.25})", &error)) << error;
    ASSERT_TRUE(CommitCapturedAddColorGrade(
        history, handle, EditorWorkingPreview(*pipeline, element_id)->Document(),
        alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.recovered"}, &error))
        << error;
    EXPECT_EQ(EditorHistoryGraph(history, element_id)->CommitCount(), 2u);
    const auto  working   = EditorWorkingPreview(*pipeline, element_id);
    const auto* recovered = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
        working->Document().Graph().FindNode(alcedo::NodeId{"grade.recovered"}));
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->DisplayName(), "Color Grade 2");
    EXPECT_EQ(working->Document().NextColorGradeNameNumber(), 3u);
    history.Release(handle);
    project.SaveProject(meta_path);
  }

  auto reopen = [&](std::size_t expected_commits) {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service = std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(
        EditorSessionPipelineMappers{[pipeline_service]() { return pipeline_service; }, {}});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    EXPECT_EQ(EditorHistoryGraph(history, element_id)->CommitCount(), expected_commits);
    EXPECT_TRUE(EditorWorkingHead(history, element_id).has_value());
    const auto working = EditorWorkingPreview(*pipeline, element_id);
    EXPECT_FLOAT_EQ(DocumentExposureEv(working->Document()), 1.25f);
    const auto* recovered = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
        working->Document().Graph().FindNode(alcedo::NodeId{"grade.recovered"}));
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->DisplayName(), "Color Grade 2");
    EXPECT_EQ(working->Document().NextColorGradeNameNumber(), 3u);
    history.Release(handle);
  };

  reopen(2u);
  {
    alcedo::MiniGitJournal journal(journal_path);
    std::string            error;
    ASSERT_TRUE(journal.Load(&error)) << error;
    EXPECT_TRUE(journal.records().empty());
  }
  reopen(2u);

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortProjectTest, ProjectReopenPreservesDagVersionsHistoryAndMasks) {
  const auto db_path      = NodeHistoryPath("reopen_versions", ".db");
  const auto meta_path    = NodeHistoryPath("reopen_versions", ".json");
  const auto journal_path = NodeHistoryPath("reopen_versions", ".wal");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 832;
  std::string               saved_hash;
  alcedo::version_ref_id_t  default_id{};
  alcedo::head_commit_hash_t saved_head;
  alcedo::transaction_chain_hash_t saved_chain{};
  std::size_t               saved_refs = 0;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    pipeline_service->InitializeImageRoot(element_id, alcedo::CreateDefaultPipelineDocument(),
                                          nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(
        EditorSessionPipelineMappers{[pipeline_service]() { return pipeline_service; }, {}});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    ASSERT_TRUE(history.ReadActiveVersionId(handle, &default_id, &error)) << error;
    ASSERT_TRUE(CommitSettled(history, handle, "exposure", R"({"exposure":0.9})", &error)) << error;
    alcedo::RadialMaskSource radial;
    radial.center_x     = 0.3f;
    radial.major_radius = 0.45f;
    ASSERT_TRUE(history.AddMask(
        handle, alcedo::NodeId{"grade.primary"},
        alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}, radial), 0, &error))
        << error;
    alcedo::version_ref_id_t root_version{};
    ASSERT_TRUE(history.CreateRootVersionAndCheckout(handle, "Clean", &root_version, &error))
        << error;
    ASSERT_TRUE(history.CheckoutVersion(handle, default_id, &error)) << error;
    saved_hash = alcedo::CanonicalPipelineDocumentJson(
        EditorWorkingPreview(*pipeline, element_id)->Document());
    saved_head  = EditorWorkingHead(history, element_id);
    saved_chain = EditorWorkingChain(history, element_id);
    saved_refs  = EditorHistoryGraph(history, element_id)->GetAllVersionRefs().size();
    // The checkout persists the checkpoint of the checked-out head's document.
    const auto checkout_checkpoint = StoredCheckpoint(project, element_id);
    ASSERT_TRUE(checkout_checkpoint.has_value());
    EXPECT_EQ(checkout_checkpoint->head_commit_hash, saved_head);
    EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(checkout_checkpoint->document), saved_hash);
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
    history.Release(handle);
    project.SaveProject(meta_path);
  }

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service = std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(
        EditorSessionPipelineMappers{[pipeline_service]() { return pipeline_service; }, {}});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    const auto graph   = EditorHistoryGraph(history, element_id);
    const auto working = EditorWorkingPreview(*pipeline, element_id);
    ASSERT_NE(graph, nullptr);
    ASSERT_NE(working, nullptr);
    EXPECT_EQ(graph->GetActiveVersionId(), default_id);
    ASSERT_NE(working->Document().PrimaryGrade(), nullptr);
    EXPECT_EQ(working->Document().PrimaryGrade()->DisplayName(), "Color Grade 1");
    EXPECT_EQ(working->Document().NextColorGradeNameNumber(), 2u);
    EXPECT_EQ(EditorWorkingHead(history, element_id), saved_head);
    EXPECT_EQ(EditorWorkingChain(history, element_id), saved_chain);
    EXPECT_EQ(graph->GetAllVersionRefs().size(), saved_refs);
    EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(working->Document()), saved_hash);
    const auto* mask = working->Document().PrimaryGrade()->FindMask(alcedo::MaskId{"mask.radial"});
    ASSERT_NE(mask, nullptr);
    const auto* radial_source = std::get_if<alcedo::RadialMaskSource>(&mask->source);
    ASSERT_NE(radial_source, nullptr);
    EXPECT_EQ(radial_source->center_x, 0.3f);
    EXPECT_EQ(radial_source->major_radius, 0.45f);
    history.Release(handle);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortProjectTest, MissingReachableTypedCommitFailsClosed) {
  const auto db_path   = NodeHistoryPath("missing_typed_commit", ".db");
  const auto meta_path = NodeHistoryPath("missing_typed_commit", ".json");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 833;
  alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
  alcedo::PipelineMgmtService first(project.GetStorage());
  first.InitializeImageRoot(element_id, alcedo::CreateDefaultPipelineDocument(), nullptr);

  alcedo::commit_hash_t missing_hash{};
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    alcedo::CommitGraphStore graph_service(db_guard.conn_);
    auto             graph = graph_service.LoadGraph(element_id);
    ASSERT_TRUE(graph.has_value());
    auto target = alcedo::test::ColorGradeFieldTarget("exposure");
    const auto batch = alcedo::MakeSetParameterBatch(
        target, nlohmann::json{{"exposure_ev", 0.0}}, nlohmann::json{{"exposure_ev", 1.5}}, true,
        true, "grade.primary");
    auto commit = alcedo::EditCommit::MakePipelineEdit(graph->GetRootId(), std::nullopt, batch);
    missing_hash = commit.GetCommitHash();
    ASSERT_TRUE(graph->InsertCommit(std::move(commit)));
    graph->MoveWorkingHead(graph->GetActiveVersionId(), missing_hash);
    graph_service.Materialize(graph->CaptureMaterialization());

    duckdb_result result;
    ASSERT_EQ(duckdb_query(db_guard.conn_,
                           std::format("DELETE FROM EditCommit WHERE commit_hash='{}';",
                                       missing_hash.ToString())
                               .c_str(),
                           &result),
              DuckDBSuccess);
    duckdb_destroy_result(&result);
  }

  alcedo::PipelineMgmtService reopened(project.GetStorage());
  try {
    (void)reopened.AcquireEditorLease(element_id);
    FAIL() << "expected missing typed first-parent commit to reject editor open";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("missing"), std::string::npos);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
}

}  // namespace
}  // namespace alcedo::ui
