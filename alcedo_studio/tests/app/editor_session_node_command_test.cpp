//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
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

  std::vector<EditorRenderRequest> requests;
  std::uint64_t                    next_job = 0;
};

/// Stateful history boundary for testing session admission/publication, not document replay.
class NodeLockHistoryPort final : public test::ControllableEditorHistoryPort {
 public:
  auto SetColorGradeDeletionProtected(const EditorHistoryGuardHandle&, const NodeId&,
                                      bool protected_value, std::string* error,
                                      bool* changed = nullptr) -> bool override {
    if (changed) *changed = false;
    if (fail_node_command) {
      if (error) *error = "mini-Git journal append failed";
      return false;
    }
    last_render_reason = std::nullopt;
    if (deletion_protected == protected_value) return true;
    deletion_protected = protected_value;
    if (changed) *changed = true;
    return true;
  }
  bool deletion_protected = true;
};

class EditorSessionNodeCommandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_          = std::make_shared<NodeLockHistoryPort>();
    pipeline_         = std::make_shared<test::FakeEditorPipelinePort>();
    tasks_            = std::make_shared<test::FakeEditorTaskPort>();
    scheduler_        = std::make_shared<RecordingNodeCommandScheduler>();
    checkpoint_store_ = std::make_shared<test::OrderRecordingCheckpointStore>();
    thumbnails_       = std::make_shared<test::FakeEditorThumbnailPort>();
    runtime_          = EditorSessionRuntime::CreateWithPorts(
        pipeline_, history_, tasks_, scheduler_, checkpoint_store_, thumbnails_);
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

  auto SampleTopologyChange() const -> NodeGraphTopologyChange {
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
    return change;
  }

  std::shared_ptr<NodeLockHistoryPort>                 history_;
  std::shared_ptr<test::FakeEditorPipelinePort>        pipeline_;
  std::shared_ptr<test::FakeEditorTaskPort>            tasks_;
  std::shared_ptr<RecordingNodeCommandScheduler>       scheduler_;
  std::shared_ptr<test::OrderRecordingCheckpointStore>     checkpoint_store_;
  std::shared_ptr<test::FakeEditorThumbnailPort>       thumbnails_;
  std::unique_ptr<EditorSessionRuntime>                runtime_;
  EditorSessionService*                                service_ = nullptr;
};

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

// E5 of the executor ownership audit: each publication used to deep-copy the live document for
// the GUI. The GUI now reads the preview the history publishes after each write: it shares
// unchanged nodes with the working document, and a later edit copies only the node it changes,
// so the earlier publication keeps its values.
TEST_F(EditorSessionNodeCommandTest, PublishedDocumentSharesNodesAndKeepsValuesAfterLiveEdit) {
  pipeline_->working_document = std::make_shared<EditorWorkingDocument>(
      10, std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument()));
  auto& live = pipeline_->working_document->Document();

  ASSERT_EQ(service_->RenameColorGrade(NodeId{"grade.primary"}, "Sky").kind,
            EditorSessionResultKind::Accepted);
  service_->DrainCommandQueueForTests();
  const auto first = service_->pipeline_document();
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(first->Graph().NodeCount(), live.Graph().NodeCount());
  for (std::size_t index = 0; index < live.Graph().NodeCount(); ++index) {
    EXPECT_EQ(first->Graph().Nodes()[index].get(), live.Graph().Nodes()[index].get());
  }
  const auto first_json = first->ToJson();

  // The history port writes the live document on the owner thread; the test is that thread.
  auto* exposure = dynamic_cast<ExposureModel*>(
      live.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  const float edited_ev = std::as_const(*first).PrimaryGrade()->FindAdjustmentByType(
                              type_ids::Exposure())->ToJson().at("exposure_ev").get<float>() +
                          0.75f;
  exposure->SetValue(edited_ev);
  (void)pipeline_->working_document->PublishPreview();

  ASSERT_EQ(service_->RenameColorGrade(NodeId{"grade.primary"}, "Sea").kind,
            EditorSessionResultKind::Accepted);
  service_->DrainCommandQueueForTests();
  const auto second = service_->pipeline_document();
  ASSERT_NE(second, nullptr);
  ASSERT_NE(second, first);

  EXPECT_EQ(first->ToJson(), first_json);
  EXPECT_FLOAT_EQ(second->PrimaryGrade()
                      ->FindAdjustmentByType(type_ids::Exposure())
                      ->ToJson()
                      .at("exposure_ev")
                      .get<float>(),
                  edited_ev);
  EXPECT_EQ(first->Develop(), second->Develop());
  EXPECT_EQ(first->Drt(), second->Drt());
  EXPECT_NE(first->PrimaryGrade(), second->PrimaryGrade());
}

TEST_F(EditorSessionNodeCommandTest, DeletionLockPublishesOnlyEffectiveChangesWithoutRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  history_->last_render_reason = EditorRenderReason::GraphTopologyChanged;

  EXPECT_EQ(service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, true).kind,
            EditorSessionResultKind::Accepted);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_FALSE(history_->LastPublishedRenderReason().has_value());
  EXPECT_EQ(scheduler_->requests.size(), renders_before);

  EXPECT_EQ(service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, false).kind,
            EditorSessionResultKind::Accepted);
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
  EXPECT_EQ(service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, false).kind,
            EditorSessionResultKind::Accepted);
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  EXPECT_EQ(service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, true).kind,
            EditorSessionResultKind::Accepted);
  EXPECT_EQ(service_->history_revision(), revision_before + 2);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest, FailedDeletionLockDoesNotPublishHistoryOrRender) {
  history_->fail_node_command = true;
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result = service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, false);
  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest, DeletionLockRejectsANonInteractiveSession) {
  (void)service_->Shutdown();
  service_->DrainCommandQueueForTests();
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  EXPECT_EQ(service_->SetColorGradeDeletionProtected(NodeId{"grade.primary"}, false).kind,
            EditorSessionResultKind::Rejected);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest,
       EditNodeGraphCreatesOneHistoryChangeAndRoutesTopologyQualityRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result          = service_->EditNodeGraph(SampleTopologyChange());

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->edit_node_graph_count, 1);
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       JournalFailurePublishesExactErrorWithoutHistoryOrRenderChange) {
  history_->fail_node_command = true;
  const auto renders_before   = scheduler_->requests.size();
  const auto revision_before  = service_->history_revision();
  const auto result           = service_->EditNodeGraph(SampleTopologyChange());

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest,
       InsertColorGradeAtTopCreatesOneHistoryChangeAndRoutesTopologyRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result = service_->InsertColorGradeAtTop(NodeId{"grade.new"}, NodeId{"grade.primary"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->insert_grade_top_count, 1);
  EXPECT_EQ(history_->last_insert_new_id, NodeId{"grade.new"});
  EXPECT_EQ(history_->last_expected_predecessor, NodeId{"grade.primary"});
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       RemoveColorGradeAndBridgeCreatesOneHistoryChangeAndRoutesTopologyRender) {
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();
  const auto result          = service_->RemoveColorGradeAndBridge(NodeId{"grade.primary"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::RenderRouted);
  EXPECT_EQ(history_->remove_grade_count, 1);
  EXPECT_EQ(history_->last_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(service_->history_revision(), revision_before + 1);
  ASSERT_EQ(scheduler_->requests.size(), renders_before + 1);
  EXPECT_EQ(scheduler_->requests.back().intent.reason, EditorRenderReason::GraphTopologyChanged);
}

TEST_F(EditorSessionNodeCommandTest,
       InsertColorGradeAtTopJournalFailureLeavesHistoryAndRenderUntouched) {
  history_->fail_node_command = true;
  const auto renders_before   = scheduler_->requests.size();
  const auto revision_before  = service_->history_revision();
  const auto result = service_->InsertColorGradeAtTop(NodeId{"grade.new"}, NodeId{"grade.primary"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->insert_grade_top_count, 1);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest,
       RemoveColorGradeAndBridgeJournalFailureLeavesHistoryAndRenderUntouched) {
  history_->fail_node_command = true;
  const auto renders_before   = scheduler_->requests.size();
  const auto revision_before  = service_->history_revision();
  const auto result           = service_->RemoveColorGradeAndBridge(NodeId{"grade.primary"});

  EXPECT_EQ(result.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(result.message, "mini-Git journal append failed");
  EXPECT_EQ(history_->remove_grade_count, 1);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

TEST_F(EditorSessionNodeCommandTest, MaskGroupCommandsRejectANonInteractiveSession) {
  (void)service_->Shutdown();
  service_->DrainCommandQueueForTests();
  const auto renders_before  = scheduler_->requests.size();
  const auto revision_before = service_->history_revision();

  const auto insert = service_->InsertColorGradeAtTop(NodeId{"grade.new"}, NodeId{"grade.primary"});
  const auto remove = service_->RemoveColorGradeAndBridge(NodeId{"grade.primary"});
  EXPECT_EQ(insert.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(remove.kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(history_->insert_grade_top_count, 0);
  EXPECT_EQ(history_->remove_grade_count, 0);
  EXPECT_EQ(service_->history_revision(), revision_before);
  EXPECT_EQ(scheduler_->requests.size(), renders_before);
}

}  // namespace
}  // namespace alcedo
