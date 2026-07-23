//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_commit_writer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto MakeSerializedState(float exposure) -> nlohmann::json {
  auto params                      = nlohmann::json::object();
  params["state_format_version"]   = 1;
  params["root_id"]                = "010000000000000001000000000000000100000000000000";
  params["head_commit_hash"]       = "";
  params["transaction_chain_hash"] = "010000000000000001000000000000000100000000000000";
  auto pipeline_params             = nlohmann::json::object();
  pipeline_params["exposure"]      = exposure;
  params["pipeline_params"]        = pipeline_params;
  return params;
}

}  // namespace

class EditorMiniGitCommitWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    db_path_        = temp / ("mini_git_writer_" + stamp + ".db");
    meta_path_      = temp / ("mini_git_writer_" + stamp + ".json");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);

    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
    storage_ = project_->GetStorageService();
    writer_  = std::make_unique<EditorMiniGitCommitWriter>(storage_);
    {
      auto               guard = storage_->GetDBController().GetConnectionGuard();
      auto               lock  = guard.Lock();
      CommitGraphService graph_service(guard.conn_);
      graph_ =
          std::make_shared<CommitGraph>(graph_service.CreateEmptyPersisted(element_id_, "Default"));
    }
  }

  void TearDown() override {
    writer_.reset();
    storage_.reset();
    project_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }

  sl_element_id_t                            element_id_ = 42;
  std::filesystem::path                      db_path_;
  std::filesystem::path                      meta_path_;
  std::unique_ptr<ProjectService>            project_;
  std::shared_ptr<StorageService>            storage_;
  std::shared_ptr<CommitGraph>               graph_;
  std::unique_ptr<EditorMiniGitCommitWriter> writer_;
};

TEST_F(EditorMiniGitCommitWriterTest, WriteEmptyGraphSucceeds) {
  auto materialization =
      graph_->CaptureMaterializationWithSerializedPipelineState(MakeSerializedState(0.0f));
  std::string error;
  auto        result = writer_->Write(materialization, &error);
  ASSERT_TRUE(result.accepted) << result.error;

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->CommitCount(), 0u);
}

TEST_F(EditorMiniGitCommitWriterTest, WriteWithSerializedStatePersistsAfterReopen) {
  auto serialized      = MakeSerializedState(1.5f);
  auto materialization = graph_->CaptureMaterializationWithSerializedPipelineState(serialized);
  std::string error;
  auto        result = writer_->Write(materialization, &error);
  ASSERT_TRUE(result.accepted) << result.error;

  auto               guard = storage_->GetDBController().GetConnectionGuard();
  auto               lock  = guard.Lock();
  CommitGraphService graph_service(guard.conn_);
  auto               stored = graph_service.LoadGraph(element_id_);
  ASSERT_TRUE(stored.has_value());
  ASSERT_TRUE(stored->GetImageEditState().serialized_pipeline_state.has_value());
  EXPECT_FLOAT_EQ(stored->GetImageEditState()
                      .serialized_pipeline_state->at("pipeline_params")
                      .at("exposure")
                      .get<float>(),
                  1.5f);
}

TEST_F(EditorMiniGitCommitWriterTest, WriteInvalidMaterializationReturnsNotAccepted) {
  CommitGraphMaterialization invalid;
  // Missing element_id, root_id, no version refs, no ImageEditState.

  std::string                error;
  auto                       result = writer_->Write(invalid, &error);
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(EditorMiniGitCommitWriterTest, WriteWithNullErrorDoesNotCrash) {
  auto materialization =
      graph_->CaptureMaterializationWithSerializedPipelineState(MakeSerializedState(0.0f));
  auto result = writer_->Write(materialization, nullptr);
  EXPECT_TRUE(result.accepted);
}

}  // namespace alcedo
