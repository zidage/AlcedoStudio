//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "support/editor_session_command_queue_test_support.hpp"

namespace alcedo {
namespace {

class RecordingNodeCommandScheduler final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request, EditorPipelineScheduleCompletion = {})
      -> std::uint64_t override {
    requests.push_back(request);
    return ++next_job;
  }
  void                             Cancel(std::uint64_t) override {}
  void                             WaitForSessionIdle(std::uint64_t) override {}

  std::vector<EditorRenderRequest> requests;
  std::uint64_t                    next_job = 0;
};

class EditorSessionNodeCommandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<test::ControllableEditorHistoryPort>();
    pipeline_         = std::make_shared<test::FakeEditorPipelinePort>();
    tasks_            = std::make_shared<test::FakeEditorTaskPort>();
    journal_          = std::make_shared<test::OrderRecordingJournalPort>();
    scheduler_        = std::make_shared<RecordingNodeCommandScheduler>();
    checkpoint_store_ = std::make_shared<test::FakeEditorCheckpointStore>();
    thumbnails_       = std::make_shared<test::FakeEditorThumbnailPort>();
    runtime_          = EditorSessionRuntime::CreateWithPorts(pipeline_, history_, tasks_, journal_,
                                                              scheduler_, checkpoint_store_, thumbnails_);
    service_          = runtime_->service.get();
    service_->SetPresentationSinkId(1);
    service_->SetPresentationSize(640, 480);
    OpenInteractive();
  }

  void OpenInteractive() {
    (void)service_->Open(10, 20);
    service_->DrainCommandQueueForTests();
    const auto first = service_->first_frame_request_id();
    ASSERT_NE(first, 0u);
    runtime_->coordinator->NotifySchedulerCompleted(first, true);
    service_->DrainCommandQueueForTests();
    const auto quality = runtime_->coordinator->last_scheduled_request_id();
    if (quality != first) {
      runtime_->coordinator->NotifySchedulerCompleted(quality, true);
      service_->DrainCommandQueueForTests();
    }
    ASSERT_EQ(service_->state(), EditorSessionState::Interactive);
  }

  std::shared_ptr<test::ControllableEditorHistoryPort> history_;
  std::shared_ptr<test::FakeEditorPipelinePort>        pipeline_;
  std::shared_ptr<test::FakeEditorTaskPort>            tasks_;
  std::shared_ptr<test::OrderRecordingJournalPort>     journal_;
  std::shared_ptr<RecordingNodeCommandScheduler>       scheduler_;
  std::shared_ptr<test::FakeEditorCheckpointStore>     checkpoint_store_;
  std::shared_ptr<test::FakeEditorThumbnailPort>       thumbnails_;
  std::unique_ptr<EditorSessionRuntime>                runtime_;
  EditorSessionService*                                service_ = nullptr;
};

TEST_F(EditorSessionNodeCommandTest, AddCreatesOneHistoryChangeAndRoutesTopologyQualityRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result          = service_->AddColorGrade(NodeId{"drt"}, NodeId{"grade.extra"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->add_grade_count, 1);
  EXPECT_EQ(history_->last_before_node_id, NodeId{"drt"});
  EXPECT_EQ(history_->last_node_id, NodeId{"grade.extra"});
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest, RenameCreatesOneHistoryChangeWithoutRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result          = service_->RenameColorGrade(NodeId{"grade.primary"}, "Sky");

  EXPECT_EQ(result.kind, EditorSessionResultKind::Accepted);
  EXPECT_EQ(history_->rename_grade_count, 1);
  EXPECT_EQ(history_->last_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(history_->last_grade_name, "Sky");
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest, DeleteCreatesOneHistoryChangeAndRoutesTopologyQualityRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result          = service_->RemoveColorGrade(NodeId{"grade.primary"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->remove_grade_count, 1);
  EXPECT_EQ(history_->last_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       EditNodeGraphCreatesOneHistoryChangeAndRoutesTopologyQualityRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  NodeGraphTopologyChange change;
  change.before_next_color_grade_name_number = 2;
  change.after_next_color_grade_name_number  = 2;
  NodeGraphDisconnectedEdge disconnected;
  disconnected.edge                = PipelineSceneEdge{NodeId{"grade.primary"}, PortId{"image"},
                                                       NodeId{"drt"}, PortId{"image"}};
  disconnected.original_edge_index = 1;
  change.disconnected_edges.push_back(disconnected);
  NodeGraphConnectedEdge connected;
  connected.edge             = PipelineSceneEdge{NodeId{"grade.primary"}, PortId{"image"},
                                                 NodeId{"drt"}, PortId{"image"}};
  connected.final_edge_index = 1;
  change.connected_edges.push_back(connected);
  const auto result = service_->EditNodeGraph(change);

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->edit_node_graph_count, 1);
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       ReconnectCreatesOneHistoryChangeAndRoutesTopologyQualityRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result =
      service_->ReconnectColorGrade(NodeId{"grade.primary"}, NodeId{"develop"}, NodeId{"drt"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->reconnect_grade_count, 1);
  EXPECT_EQ(history_->last_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(history_->last_predecessor_id, NodeId{"develop"});
  EXPECT_EQ(history_->last_successor_id, NodeId{"drt"});
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       JournalFailurePublishesExactErrorWithoutHistoryOrRenderChange) {
  history_->fail_node_command = true;
  const auto renders_before   = scheduler_->requests.size();
  const auto revision_before  = service_->history_revision();
  const auto result           = service_->AddColorGrade(NodeId{"drt"}, NodeId{"grade.extra"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

}  // namespace
}  // namespace alcedo
