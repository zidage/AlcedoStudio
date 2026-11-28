//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// AI Denoise integration plan, DN8: the switch written into the stored history of an image that
// the editor does not hold (Turn Off AI Denoise of an export queue item).

#include "app/ai_denoise_history_write.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/pipeline_document_history.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "json.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

constexpr sl_element_id_t kImage = 801;

auto                      ModelVersion(char digit) -> DevelopAiDenoise {
  return DevelopAiDenoise{std::string(64, digit)};
}

/// The document of the active Version head as storage holds it.
auto StoredHeadDocument(PipelineMgmtService& pipelines, sl_element_id_t element_id)
    -> std::shared_ptr<PipelineDocument> {
  const auto  stored = pipelines.LoadHistorySnapshot(element_id);
  std::string error;
  auto        document = BuildDocumentFromRoot(
      *stored.graph_, *stored.root_, stored.graph_->GetActiveVersionRef().head_commit_hash, &error);
  if (!document) {
    throw std::runtime_error("replay failed: " + error);
  }
  return document;
}

auto StoredDenoise(PipelineMgmtService& pipelines, sl_element_id_t element_id)
    -> std::optional<DevelopAiDenoise> {
  return StoredHeadDocument(pipelines, element_id)->Develop()->Params().AiDenoise();
}

auto StoredExposure(PipelineMgmtService& pipelines, sl_element_id_t element_id) -> float {
  const auto  document = StoredHeadDocument(pipelines, element_id);
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      document->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  if (exposure == nullptr) {
    throw std::runtime_error("document is missing exposure");
  }
  return exposure->Value();
}

auto StoredGraph(PipelineMgmtService& pipelines, sl_element_id_t element_id) -> CommitGraph {
  return *pipelines.LoadHistorySnapshot(element_id).graph_;
}

class AiDenoiseHistoryWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    project_ =
        std::make_unique<ProjectService>(temp_dir_ / "project.db", temp_dir_ / "project.json");
    pipelines_ = std::make_unique<PipelineMgmtService>(project_->GetStorage());
    pipelines_->InitializeImageRoot(kImage, CreateDefaultPipelineDocument(), nullptr);
  }

  /// One stored exposure edit on the active Version, as an earlier editor session left it.
  void StoreExposureEdit(float after) {
    const auto  base   = pipelines_->LoadHistorySnapshot(kImage);
    CommitGraph graph  = *base.graph_;
    auto        commit = EditCommit::MakePipelineEdit(
        graph.GetRootId(), graph.GetActiveVersionRef().head_commit_hash,
        MakeSetParameterBatch(test::ColorGradeFieldTarget("exposure"),
                                     nlohmann::json{{"exposure_ev", kDefaultPipelineExposureEv}},
                                     nlohmann::json{{"exposure_ev", after}}, true, true, "Default"));
    const auto head = commit.GetCommitHash();
    ASSERT_TRUE(graph.InsertCommit(std::move(commit)));
    graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
    (void)pipelines_->PersistHistory(base, graph);
  }

  test::TestTempDirectory              temp_dir_;
  std::unique_ptr<ProjectService>      project_;
  std::unique_ptr<PipelineMgmtService> pipelines_;
};

TEST_F(AiDenoiseHistoryWriteTest, TurnOnWritesOneRawDecodeCommitOnTheActiveVersion) {
  StoreExposureEdit(2.5f);
  const auto before         = StoredGraph(*pipelines_, kImage);
  const auto before_head    = before.GetActiveVersionRef().head_commit_hash;
  const auto active_version = before.GetActiveVersionId();
  const auto version_count  = before.GetAllVersionRefs().size();

  const auto written = WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('a'));

  EXPECT_TRUE(written.changed);
  EXPECT_EQ(written.version_name, before.GetActiveVersionRef().display_name);
  const auto after = StoredGraph(*pipelines_, kImage);
  EXPECT_EQ(after.GetActiveVersionId(), active_version);
  EXPECT_EQ(after.GetAllVersionRefs().size(), version_count) << "no Version is added";
  const auto head = after.GetActiveVersionRef().head_commit_hash;
  ASSERT_TRUE(head.has_value());
  const auto* commit = after.FindCommit(*head);
  ASSERT_NE(commit, nullptr);
  EXPECT_EQ(commit->GetFirstParentHash(), before_head) << "one commit on the previous head";
  EXPECT_EQ(static_cast<std::int64_t>(commit->GetCreatedAtNs()), written.head_created_at_ns);
  EXPECT_EQ(StoredDenoise(*pipelines_, kImage), ModelVersion('a'));
  EXPECT_FLOAT_EQ(StoredExposure(*pipelines_, kImage), 2.5f) << "the earlier edit stays";
}

TEST_F(AiDenoiseHistoryWriteTest, WriteOfTheSameValueIsUnchanged) {
  const auto root_head = StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash;
  const auto off       = WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, std::nullopt);
  EXPECT_FALSE(off.changed) << "the switch of a new image is off";
  EXPECT_EQ(StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash, root_head);

  ASSERT_TRUE(WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('b')).changed);
  const auto on_head = StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash;
  const auto again   = WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('b'));
  EXPECT_FALSE(again.changed);
  EXPECT_EQ(StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash, on_head);

  // Another installed version is a change: the parameter records the installed version (D10).
  EXPECT_TRUE(WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('c')).changed);
  EXPECT_EQ(StoredDenoise(*pipelines_, kImage), ModelVersion('c'));
}

TEST_F(AiDenoiseHistoryWriteTest, TurnOffRemovesTheKeyFromTheStoredDocument) {
  ASSERT_TRUE(WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('d')).changed);
  ASSERT_NE(StoredHeadDocument(*pipelines_, kImage)->ToJson().dump().find("ai_denoise"),
            std::string::npos);

  const auto off = WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, std::nullopt);

  EXPECT_TRUE(off.changed);
  EXPECT_EQ(StoredDenoise(*pipelines_, kImage), std::nullopt);
  // The Model JSON has the key only while the switch is on (DN5), so the document of the switch
  // off is the document before the switch went on.
  EXPECT_EQ(StoredHeadDocument(*pipelines_, kImage)->ToJson().dump().find("ai_denoise"),
            std::string::npos);
}

TEST_F(AiDenoiseHistoryWriteTest, WriteRefusesAnImageThatTheEditorHolds) {
  const auto head  = StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash;
  auto       lease = pipelines_->AcquireEditorLease(kImage);
  ASSERT_NE(lease.document_, nullptr);

  EXPECT_THROW((void)WriteAiDenoiseSwitchToStoredHistory(*pipelines_, kImage, ModelVersion('e')),
               std::runtime_error);

  pipelines_->ReleaseEditorLease(kImage);
  EXPECT_EQ(StoredGraph(*pipelines_, kImage).GetActiveVersionRef().head_commit_hash, head);
  EXPECT_EQ(StoredDenoise(*pipelines_, kImage), std::nullopt);
}

}  // namespace
}  // namespace alcedo
