//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <duckdb.h>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"

#include "app/editor_node_graph_projection.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_render_intent.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "grade_owned_mask_support.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "type/hash_type.hpp"

namespace alcedo::ui {
namespace {

auto NodeHistoryPath(std::string_view name, std::string_view ext) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/node_history"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + std::string{ext});
}

auto MakeGuard(sl_element_id_t element_id) -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  guard->document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
  guard->commit_graph_ =
      std::make_shared<alcedo::CommitGraph>(alcedo::CommitGraph::CreateEmpty(element_id));
  guard->root_id_ = guard->commit_graph_->GetRootId();
  guard->root_document_ =
      std::make_shared<alcedo::PipelineDocument>(alcedo::ClonePipelineDocument(*guard->document_));
  return guard;
}

auto ColorGradeTarget(const std::string& field) -> alcedo::EditorParameterTarget {
  return alcedo::test::ColorGradeFieldTarget(field);
}

auto CommitSettled(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                   const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview{field, after_json, false};
  alcedo::EditorAdjustmentPatch settled{field, after_json, true};
  if (field == "exposure") {
    auto params = nlohmann::json::parse(after_json);
    if (params.contains("exposure")) {
      params["exposure_ev"] = params.at("exposure");
      params.erase("exposure");
      preview.params_json = params.dump();
      settled.params_json = params.dump();
    }
  }
  preview = alcedo::test::WithColorGradeTarget(std::move(preview));
  settled = alcedo::test::WithColorGradeTarget(std::move(settled));
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto CommitPanelField(EditorSessionHistoryPort& port, const alcedo::EditorHistoryGuardHandle& handle,
                      const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  alcedo::EditorAdjustmentPatch preview{field, after_json, false};
  alcedo::EditorAdjustmentPatch settled{field, after_json, true};
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto DocumentHash(const alcedo::PipelineGuard& guard) -> std::string {
  return alcedo::CanonicalPipelineDocumentJson(*guard.document_);
}

auto DocumentExposureEv(const alcedo::PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(alcedo::ReadEditorParameterJson(document, ColorGradeTarget("exposure"), &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

class EditorVersionCheckoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    journal_path_ = NodeHistoryPath("version_checkout", ".wal");
    guard_        = MakeGuard(42);
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
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
};

TEST_F(EditorVersionCheckoutTest, RootVersionAlwaysRebuildsExactImmutableDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto root_hash = alcedo::CanonicalPipelineDocumentJson(*guard_->root_document_);
  ASSERT_TRUE(CommitPanelField(history_, handle, "crop_rotate", R"({"rotation_degrees":12.0})",
                               &error))
      << error;
  ASSERT_TRUE(CommitPanelField(history_, handle, "color_temp",
                               R"({"custom_cct":7200.0,"wb_mode":"custom"})", &error))
      << error;
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":2.25})", &error)) << error;
  ASSERT_TRUE(CommitPanelField(history_, handle, "clarity", R"({"clarity":18.0})", &error))
      << error;
  ASSERT_TRUE(history_.AddColorGrade(handle, alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.look"},
                                     &error))
      << error;
  ASSERT_TRUE(history_.AddMask(handle, alcedo::NodeId{"grade.look"},
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}),
                               0, &error))
      << error;
  EXPECT_NE(DocumentHash(*guard_), root_hash);
  EXPECT_NE(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);

  alcedo::version_ref_id_t root_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Root", &root_version, &error))
      << error;
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), root_version);
  EXPECT_FALSE(guard_->working_head_commit_hash().has_value());
  EXPECT_EQ(DocumentHash(*guard_), root_hash);
  EXPECT_EQ(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), DocumentExposureEv(*guard_->root_document_));
  ASSERT_TRUE(history_.LastPublishedRenderReason().has_value());
  EXPECT_EQ(*history_.LastPublishedRenderReason(), alcedo::EditorRenderReason::VersionDocumentChanged);
}

TEST_F(EditorVersionCheckoutTest, BranchVersionSharesCommitsAndKeepsIndependentHead) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = guard_->commit_graph_->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.75})", &error)) << error;
  const auto shared_head = guard_->working_head_commit_hash();
  ASSERT_TRUE(shared_head.has_value());
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 1u);

  alcedo::version_ref_id_t branch_id{};
  ASSERT_TRUE(history_.BranchFromCommitAndCheckout(handle, *shared_head, "Look B", &branch_id,
                                                   &error))
      << error;
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 1u);
  EXPECT_EQ(guard_->commit_graph_->GetAllVersionRefs().size(), 2u);
  EXPECT_EQ(guard_->working_head_commit_hash(), shared_head);

  ASSERT_TRUE(history_.AddColorGrade(handle, alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.look"},
                                     &error))
      << error;
  ASSERT_TRUE(history_.AddMask(handle, alcedo::NodeId{"grade.look"},
                               alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}),
                               0, &error))
      << error;
  const auto branch_head = guard_->working_head_commit_hash();
  ASSERT_TRUE(branch_head.has_value());
  EXPECT_NE(*branch_head, *shared_head);
  EXPECT_EQ(guard_->commit_graph_->CommitCount(), 3u);
  EXPECT_NE(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  const auto* branch_grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}));
  ASSERT_NE(branch_grade, nullptr);
  EXPECT_EQ(branch_grade->DisplayName(), "Color Grade 2");
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 3u);

  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), shared_head);
  EXPECT_EQ(guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}), nullptr);
  EXPECT_FLOAT_EQ(DocumentExposureEv(*guard_->document_), 0.75f);
  ASSERT_NE(guard_->document_->PrimaryGrade(), nullptr);
  EXPECT_EQ(guard_->document_->PrimaryGrade()->DisplayName(), "Color Grade 1");
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 2u);
  const auto default_projection =
      alcedo::EditorNodeGraphProjection::Build(*guard_->document_, 8, 11, 1);
  ASSERT_EQ(default_projection.nodes.size(), 3u);
  EXPECT_EQ(default_projection.nodes[1].node_id, alcedo::NodeId{"grade.primary"});
  EXPECT_EQ(default_projection.nodes[1].display_name, "Color Grade 1");

  ASSERT_TRUE(history_.CheckoutVersion(handle, branch_id, &error)) << error;
  EXPECT_EQ(guard_->working_head_commit_hash(), branch_head);
  const auto* checked_out_grade = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
      guard_->document_->Graph().FindNode(alcedo::NodeId{"grade.look"}));
  ASSERT_NE(checked_out_grade, nullptr);
  EXPECT_EQ(checked_out_grade->DisplayName(), "Color Grade 2");
  EXPECT_EQ(guard_->document_->NextColorGradeNameNumber(), 3u);
  const auto branch_projection =
      alcedo::EditorNodeGraphProjection::Build(*guard_->document_, 8, 12, 2);
  ASSERT_EQ(branch_projection.nodes.size(), 4u);
  EXPECT_EQ(branch_projection.nodes[2].node_id, alcedo::NodeId{"grade.look"});
  EXPECT_EQ(branch_projection.nodes[2].display_name, "Color Grade 2");
  EXPECT_TRUE(alcedo::EditorNodeGraphProjection::AcceptsGeneration(branch_projection, 8));
}

TEST_F(EditorVersionCheckoutTest, VersionCheckoutReplacesTheDagOnTheSameLiveGuard) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto* live        = guard_.get();
  const auto  default_id  = guard_->commit_graph_->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":2.0})", &error)) << error;
  const auto edited_hash = DocumentHash(*guard_);

  alcedo::version_ref_id_t root_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Clean", &root_version, &error))
      << error;
  const auto root_hash = DocumentHash(*guard_);
  EXPECT_NE(root_hash, edited_hash);
  EXPECT_EQ(guard_.get(), live);

  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(DocumentHash(*guard_), edited_hash);
  EXPECT_EQ(guard_.get(), live);

  ASSERT_TRUE(history_.CheckoutVersion(handle, root_version, &error)) << error;
  EXPECT_EQ(DocumentHash(*guard_), root_hash);
  ASSERT_TRUE(history_.CheckoutVersion(handle, default_id, &error)) << error;
  EXPECT_EQ(DocumentHash(*guard_), edited_hash);
  EXPECT_EQ(guard_.get(), live);
}

TEST_F(EditorVersionCheckoutTest, FailedCheckoutRestoresPriorVersionAndDocument) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id   = guard_->commit_graph_->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.5})", &error)) << error;
  const auto prior_hash   = DocumentHash(*guard_);
  const auto prior_head   = guard_->working_head_commit_hash();
  const auto prior_reason = history_.LastPublishedRenderReason();
  ASSERT_TRUE(prior_head.has_value());

  // CreateVersionRefAtHead refuses a missing hash. Point a real Version at a
  // missing first-parent so checkout fails closed without throwing.
  const auto missing_id =
      guard_->commit_graph_->CreateVersionRefAtHead("MissingHead", prior_head);
  guard_->commit_graph_->GetVersionRef(missing_id).head_commit_hash =
      alcedo::Hash128{0x11, 0x22};
  EXPECT_FALSE(history_.CheckoutVersion(handle, missing_id, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), default_id);
  EXPECT_EQ(guard_->working_head_commit_hash(), prior_head);
  EXPECT_EQ(DocumentHash(*guard_), prior_hash);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);

  auto missing_target = alcedo::test::ColorGradeFieldTarget("exposure");
  missing_target.node_id = alcedo::NodeId{"grade.does_not_exist"};
  const auto bad_batch   = alcedo::MakeSetParameterBatch(
      missing_target, nlohmann::json{{"exposure_ev", 0.0}}, nlohmann::json{{"exposure_ev", 3.0}},
      true, true, "missing");
  auto bad_commit = alcedo::EditCommit::MakePipelineEdit(guard_->commit_graph_->GetRootId(),
                                                         std::nullopt, bad_batch);
  const auto bad_hash = bad_commit.GetCommitHash();
  ASSERT_TRUE(guard_->commit_graph_->InsertCommit(std::move(bad_commit)));
  const auto bad_id = guard_->commit_graph_->CreateVersionRefAtHead("InvalidBatch", bad_hash);
  error.clear();
  EXPECT_FALSE(history_.CheckoutVersion(handle, bad_id, &error));
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), default_id);
  EXPECT_EQ(DocumentHash(*guard_), prior_hash);
  EXPECT_EQ(history_.LastPublishedRenderReason(), prior_reason);
}

TEST_F(EditorVersionCheckoutTest, MissingReachableMaskAssetFailsBeforeHeadPublication) {
  std::string error;
  const auto  handle = history_.Acquire(42, &error);
  ASSERT_TRUE(handle.valid) << error;
  const auto default_id = guard_->commit_graph_->GetActiveVersionId();
  ASSERT_TRUE(CommitSettled(history_, handle, "exposure", R"({"exposure":0.25})", &error)) << error;

  const auto mask_root = NodeHistoryPath("checkout_mask", "");
  alcedo::MaskStore store(mask_root);
  alcedo::MaskAssetDescriptor descriptor;
  descriptor.extent           = {4, 4};
  descriptor.reference_bounds = {0.0f, 0.0f, 1.0f, 1.0f};
  const std::vector<std::uint8_t> pixels(16, 90);
  const auto saved_key = store.Put(descriptor, pixels);
  ASSERT_TRUE(history_.AddMask(
      handle, alcedo::NodeId{"grade.primary"},
      alcedo::grade_mask_test::MakeBrushMask(alcedo::MaskId{"mask.brush"}, saved_key, descriptor),
      0, &error))
      << error;
  const auto after_source =
      alcedo::MaskModelToJson(alcedo::grade_mask_test::MakeBrushMask(
                                  alcedo::MaskId{"mask.brush"}, saved_key, descriptor))
          .at("source");
  ASSERT_TRUE(history_.ReplaceMaskAsset(handle, alcedo::NodeId{"grade.primary"},
                                       alcedo::MaskId{"mask.brush"}, after_source, store, &error))
      << error;

  alcedo::version_ref_id_t root_version{};
  ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle, "Clean", &root_version, &error))
      << error;
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), root_version);
  const auto root_hash = DocumentHash(*guard_);
  EXPECT_TRUE(alcedo::CollectPersistentMaskAssetKeys(*guard_->document_).empty());

  store.SetHostCacheBudget(1);
  std::error_code ignored;
  std::filesystem::remove(store.PathFor(saved_key), ignored);
  ASSERT_FALSE(std::filesystem::exists(store.PathFor(saved_key)));

  error.clear();
  EXPECT_FALSE(history_.CheckoutVersion(handle, default_id, &error));
  EXPECT_NE(error.find("Mask"), std::string::npos);
  EXPECT_EQ(guard_->commit_graph_->GetActiveVersionId(), root_version);
  EXPECT_EQ(DocumentHash(*guard_), root_hash);
  EXPECT_TRUE(alcedo::CollectPersistentMaskAssetKeys(*guard_->document_).empty());
}

TEST(EditorSessionHistoryPortProjectTest, RecoveryAppliesCommittedTypedSuffixExactlyOnce) {
  RegisterAllOperators();
  const auto db_path     = NodeHistoryPath("typed_wal_recovery", ".db");
  const auto meta_path   = NodeHistoryPath("typed_wal_recovery", ".json");
  const auto journal_path = NodeHistoryPath("typed_wal_recovery", ".wal");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 831;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; }, [guard](sl_element_id_t) { return guard; }});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    ASSERT_TRUE(CommitSettled(history, handle, "exposure", R"({"exposure":1.25})", &error)) << error;
    ASSERT_TRUE(history.AddColorGrade(handle, alcedo::NodeId{"drt"},
                                      alcedo::NodeId{"grade.recovered"}, &error))
        << error;
    EXPECT_EQ(guard->commit_graph_->CommitCount(), 2u);
    const auto* recovered = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
        guard->document_->Graph().FindNode(alcedo::NodeId{"grade.recovered"}));
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->DisplayName(), "Color Grade 2");
    EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 3u);
    history.Release(handle);
    project.SaveProject(meta_path);
  }

  auto reopen = [&](std::size_t expected_commits) {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kLoadExisting);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; }, [guard](sl_element_id_t) { return guard; }});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    EXPECT_EQ(guard->commit_graph_->CommitCount(), expected_commits);
    EXPECT_TRUE(guard->working_head_commit_hash().has_value());
    EXPECT_FLOAT_EQ(DocumentExposureEv(*guard->document_), 1.25f);
    const auto* recovered = dynamic_cast<const alcedo::ColorGradeNodeModel*>(
        guard->document_->Graph().FindNode(alcedo::NodeId{"grade.recovered"}));
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->DisplayName(), "Color Grade 2");
    EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 3u);
    history.Release(handle);
    pipeline_service->SavePipeline(guard);
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

TEST(EditorSessionHistoryPortProjectTest, ProjectReopenPreservesDagVersionsHistoryAndMaskAssets) {
  RegisterAllOperators();
  const auto db_path      = NodeHistoryPath("reopen_versions", ".db");
  const auto meta_path    = NodeHistoryPath("reopen_versions", ".json");
  const auto journal_path = NodeHistoryPath("reopen_versions", ".wal");
  const auto mask_root    = NodeHistoryPath("reopen_masks", "");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 832;
  std::string               saved_hash;
  alcedo::version_ref_id_t  default_id{};
  alcedo::head_commit_hash_t saved_head;
  alcedo::transaction_chain_hash_t saved_chain{};
  alcedo::MaskAssetKey      saved_key;
  std::size_t               saved_refs = 0;

  {
    alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
    auto pipeline_service =
        std::make_shared<alcedo::PipelineMgmtService>(project.GetStorage());
    auto guard = pipeline_service->LoadEditorPipeline(element_id);
    ASSERT_NE(guard, nullptr);
    default_id = guard->commit_graph_->GetActiveVersionId();
    auto pipeline = std::make_shared<EditorSessionPipelinePort>();
    pipeline->SetServices(EditorSessionPipelineMappers{
        [pipeline_service]() { return pipeline_service; }, [guard](sl_element_id_t) { return guard; }});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    ASSERT_TRUE(CommitSettled(history, handle, "exposure", R"({"exposure":0.9})", &error)) << error;
    alcedo::MaskStore store(mask_root);
    alcedo::MaskAssetDescriptor descriptor;
    descriptor.extent           = {4, 4};
    descriptor.reference_bounds = {0.0f, 0.0f, 1.0f, 1.0f};
    const std::vector<std::uint8_t> pixels(16, 17);
    saved_key = store.Put(descriptor, pixels);
    ASSERT_TRUE(history.AddMask(
        handle, alcedo::NodeId{"grade.primary"},
        alcedo::grade_mask_test::MakeBrushMask(alcedo::MaskId{"mask.brush"}, saved_key, descriptor),
        0, &error))
        << error;
    const auto after_source =
        alcedo::MaskModelToJson(alcedo::grade_mask_test::MakeBrushMask(
                                    alcedo::MaskId{"mask.brush"}, saved_key, descriptor))
            .at("source");
    ASSERT_TRUE(history.ReplaceMaskAsset(handle, alcedo::NodeId{"grade.primary"},
                                         alcedo::MaskId{"mask.brush"}, after_source, store, &error))
        << error;
    alcedo::version_ref_id_t root_version{};
    ASSERT_TRUE(history.CreateRootVersionAndCheckout(handle, "Clean", &root_version, &error))
        << error;
    ASSERT_TRUE(history.CheckoutVersion(handle, default_id, &error)) << error;
    saved_hash  = alcedo::CanonicalPipelineDocumentJson(*guard->document_);
    saved_head  = guard->working_head_commit_hash();
    saved_chain = guard->transaction_chain_hash();
    saved_refs  = guard->commit_graph_->GetAllVersionRefs().size();
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
    pipeline_service->SavePipeline(guard);
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
        [pipeline_service]() { return pipeline_service; }, [guard](sl_element_id_t) { return guard; }});
    EditorSessionHistoryPort history;
    history.SetServices(EditorSessionHistoryPort::Services{
        [journal_path](sl_element_id_t) { return journal_path; }});
    history.SetPipelinePort(pipeline);
    std::string error;
    const auto  handle = history.Acquire(element_id, &error);
    ASSERT_TRUE(handle.valid) << error;
    EXPECT_EQ(guard->commit_graph_->GetActiveVersionId(), default_id);
    ASSERT_NE(guard->document_->PrimaryGrade(), nullptr);
    EXPECT_EQ(guard->document_->PrimaryGrade()->DisplayName(), "Color Grade 1");
    EXPECT_EQ(guard->document_->NextColorGradeNameNumber(), 2u);
    EXPECT_EQ(guard->working_head_commit_hash(), saved_head);
    EXPECT_EQ(guard->transaction_chain_hash(), saved_chain);
    EXPECT_EQ(guard->commit_graph_->GetAllVersionRefs().size(), saved_refs);
    EXPECT_EQ(alcedo::CanonicalPipelineDocumentJson(*guard->document_), saved_hash);
    const auto keys = alcedo::CollectPersistentMaskAssetKeys(*guard->document_);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.front(), saved_key);
    EXPECT_TRUE(std::filesystem::exists(alcedo::MaskStore(mask_root).PathFor(saved_key)));
    history.Release(handle);
    pipeline_service->SavePipeline(guard);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
  std::filesystem::remove(journal_path, ec);
}

TEST(EditorSessionHistoryPortProjectTest, MissingReachableTypedCommitFailsClosed) {
  RegisterAllOperators();
  const auto db_path   = NodeHistoryPath("missing_typed_commit", ".db");
  const auto meta_path = NodeHistoryPath("missing_typed_commit", ".json");
  std::error_code ec;
  constexpr sl_element_id_t element_id = 833;
  alcedo::ProjectService project(db_path, meta_path, alcedo::ProjectOpenMode::kCreateNew);
  alcedo::PipelineMgmtService first(project.GetStorage());
  auto initial = first.LoadEditorPipeline(element_id);
  ASSERT_NE(initial, nullptr);
  first.SavePipeline(initial);

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
    (void)reopened.LoadEditorPipeline(element_id);
    FAIL() << "expected missing typed first-parent commit to reject editor open";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("missing"), std::string::npos);
  }

  std::filesystem::remove(db_path, ec);
  std::filesystem::remove(meta_path, ec);
}

}  // namespace
}  // namespace alcedo::ui
