//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTranslator>
#include <algorithm>
#include <optional>
#include <vector>

#include "app/editor_action_policy.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "app/editor_pending_input.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "edit/frame_presentation_types.hpp"
#include "app/editor_adjustment_context.hpp"
#include "app/editor_render_intent.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "grade_owned_mask_support.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_node_graph_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace {

using alcedo::AddCleanColorGrade;
using alcedo::CreateDefaultPipelineDocument;
using alcedo::EditorNodeGraphProjection;
using alcedo::EditorPendingInputBoundaryKind;
using alcedo::EditorPendingInputView;
using alcedo::EditorParameterOwnerKind;
using alcedo::EditorScalarWrite;
using alcedo::ImageGeometryUpdate;
using alcedo::EditorSessionIdentity;
using alcedo::EditorSessionResult;
using alcedo::EditorSessionState;
using alcedo::IEditorSessionBackend;
using alcedo::NodeId;
using alcedo::PipelineDocument;
using alcedo::ui::EditorNodeController;
using alcedo::ui::EditorNodeLayoutStore;
using alcedo::ui::EditorSessionController;
using alcedo::ui::NodeIdToQString;
using alcedo::ui::PresentNodeGraphDraftIssue;

class DocumentSessionBackend final : public IEditorSessionBackend {
 public:
  DocumentSessionBackend() {
    identity_.element_id = 8;
    identity_.image_id   = 9;
    document_            = CreateDefaultPipelineDocument();
    availability_.decisions[static_cast<std::size_t>(alcedo::EditorAction::PreviewAdjustment)]
        .allowed = true;
    availability_.decisions[static_cast<std::size_t>(alcedo::EditorAction::CommitAdjustment)]
        .allowed = true;
  }

  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override { return true; }
  [[nodiscard]] auto has_image() const -> bool override { return has_image_; }
  [[nodiscard]] auto last_error() const -> std::string override { return {}; }
  [[nodiscard]] auto pipeline_document() const -> const PipelineDocument* override {
    return &document_;
  }
  [[nodiscard]] auto history_revision() const -> std::uint64_t override {
    return history_revision_;
  }
  [[nodiscard]] auto active_version_id() const -> alcedo::version_ref_id_t override {
    ++active_version_read_count_;
    return {};
  }
  [[nodiscard]] auto history_snapshot() -> alcedo::EditorHistorySnapshot override {
    ++history_snapshot_read_count_;
    return {};
  }
  [[nodiscard]] auto active_image_load_request() const -> alcedo::ImageLoadRequestId override {
    return request_;
  }
  [[nodiscard]] auto action_availability() const -> alcedo::EditorActionAvailability override {
    return availability_;
  }

  void SetActionAvailabilityObserver(
      IEditorSessionBackend::ActionAvailabilityObserver observer) override {
    availability_observer_ = std::move(observer);
  }

  void SetPresentationSinkId(alcedo::PresentationSinkId) override {}
  void SetPresentationSize(int, int) override {}
  auto SetAdjustmentProjectionNode(const NodeId& node_id) -> EditorSessionResult override {
    ++set_projection_count_;
    last_projection_node_ = node_id;
    return Accepted("Selected node panel projection updated");
  }
  auto EnqueueAdjustmentInput(alcedo::EditorAdjustmentPatch patch) -> EditorSessionResult override {
    ++enqueue_count_;
    const auto admitted = pending_input_.AdmitFieldChange(identity_, std::move(patch));
    if (!admitted.accepted) {
      return Rejected(admitted.error);
    }
    return Accepted("Adjustment input queued");
  }
  auto EnqueuePendingInputBoundary(EditorPendingInputBoundaryKind kind)
      -> EditorSessionResult override {
    ++boundary_count_;
    last_boundary_      = kind;
    const auto admitted = pending_input_.AdmitBoundary(identity_, kind);
    if (!admitted.accepted) {
      return Rejected(admitted.error);
    }
    return Accepted("Adjustment input boundary queued");
  }
  [[nodiscard]] auto PeekPendingInput() const -> EditorPendingInputView override {
    return pending_input_.Peek();
  }
  auto RequestViewChange(alcedo::EditorRenderReason, std::optional<alcedo::ViewportRenderRegion>)
      -> EditorSessionResult override {
    ++view_change_count_;
    auto result  = Accepted("View change recorded");
    result.kind  = alcedo::EditorSessionResultKind::RenderRouted;
    return result;
  }
  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    ++request_.value;
    NotifyChange();
    return Accepted("opened");
  }
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override {
    identity_.element_id = element_id;
    identity_.image_id   = image_id;
    ++request_.value;
    NotifyChange();
    return Accepted("switched");
  }
  auto Close(bool) -> EditorSessionResult override { return {}; }
  auto Shutdown() -> EditorSessionResult override { return {}; }
  auto Discard() -> EditorSessionResult override { return {}; }
  auto Undo() -> EditorSessionResult override { return {}; }
  auto Redo() -> EditorSessionResult override { return {}; }

  auto RenameColorGrade(const NodeId& node_id, std::string display_name)
      -> EditorSessionResult override {
    ++rename_count_;
    if (fail_commands_) return Rejected("mini-Git journal append failed");
    const auto errors = alcedo::RenameColorGrade(document_, node_id, std::move(display_name));
    if (!errors.empty()) return Rejected(errors.front().message);
    NotifyChange();
    return Accepted("Color Grade renamed");
  }
  auto EditNodeGraph(alcedo::NodeGraphTopologyChange change) -> EditorSessionResult override {
    ++edit_node_graph_count_;
    last_topology_change_ = change;
    if (fail_commands_) return Rejected("mini-Git journal append failed");
    const auto errors = alcedo::ApplyNodeGraphTopologyChange(
        document_, change, alcedo::PipelineEditApplyDirection::Forward);
    if (!errors.empty()) return Rejected(errors.front().message);
    NotifyChange();
    auto result = Accepted("Node graph topology updated");
    result.kind = alcedo::EditorSessionResultKind::RenderRouted;
    return result;
  }

  void SetGeneration(std::uint64_t value) { request_.value = value; }
  void SetState(EditorSessionState state, bool has_image = true) {
    state_     = state;
    has_image_ = has_image;
    NotifyChange();
  }
  void SetImageId(image_id_t image_id) {
    identity_.image_id = image_id;
    NotifyChange();
  }
  void PublishStateChange() { NotifyChange(); }
  void PublishHistoryChange() {
    ++history_revision_;
    NotifyChange();
  }
  auto Document() -> PipelineDocument& { return document_; }
  void SetFailCommands(bool fail) { fail_commands_ = fail; }
  void PublishAvailability(alcedo::EditorActionAvailability availability) {
    availability_ = std::move(availability);
    if (availability_observer_) {
      availability_observer_(availability_);
    }
  }
  [[nodiscard]] auto rename_count() const -> int { return rename_count_; }
  [[nodiscard]] auto edit_node_graph_count() const -> int { return edit_node_graph_count_; }
  [[nodiscard]] auto active_version_read_count() const -> int { return active_version_read_count_; }
  [[nodiscard]] auto history_snapshot_read_count() const -> int {
    return history_snapshot_read_count_;
  }
  [[nodiscard]] auto set_projection_count() const -> int { return set_projection_count_; }
  [[nodiscard]] auto last_projection_node() const -> NodeId { return last_projection_node_; }
  [[nodiscard]] auto enqueue_count() const -> int { return enqueue_count_; }
  [[nodiscard]] auto boundary_count() const -> int { return boundary_count_; }
  [[nodiscard]] auto view_change_count() const -> int { return view_change_count_; }
  [[nodiscard]] auto last_boundary() const -> EditorPendingInputBoundaryKind {
    return last_boundary_;
  }

 private:
  auto Accepted(std::string message) const -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = alcedo::EditorSessionResultKind::Accepted;
    result.state    = EditorSessionState::Interactive;
    result.identity = identity_;
    result.message  = std::move(message);
    return result;
  }
  auto Rejected(std::string message) const -> EditorSessionResult {
    auto result = Accepted(std::move(message));
    result.kind = alcedo::EditorSessionResultKind::Rejected;
    return result;
  }

  EditorSessionIdentity                             identity_{};
  PipelineDocument                                  document_;
  alcedo::ImageLoadRequestId                        request_{};
  alcedo::EditorActionAvailability                  availability_{};
  IEditorSessionBackend::ActionAvailabilityObserver availability_observer_;
  EditorSessionState                                state_                       = EditorSessionState::Interactive;
  bool                                              has_image_                   = true;
  bool                                              fail_commands_               = false;
  int                                               rename_count_                = 0;
  int                                               edit_node_graph_count_       = 0;
  std::uint64_t                                     history_revision_            = 0;
  mutable int                                       active_version_read_count_   = 0;
  int                                               history_snapshot_read_count_ = 0;
  int                                               set_projection_count_        = 0;
  int                                               enqueue_count_               = 0;
  int                                               boundary_count_              = 0;
  int                                               view_change_count_           = 0;
  NodeId                                            last_projection_node_;
  EditorPendingInputBoundaryKind                    last_boundary_ = EditorPendingInputBoundaryKind::None;
  alcedo::EditorPendingInputQueue                   pending_input_;
  alcedo::NodeGraphTopologyChange                   last_topology_change_{};
};

TEST(EditorNodeController, LayoutStoreBindingDoesNotPublishASnapshot) {
  EditorNodeController  controller;
  EditorNodeLayoutStore store;
  EXPECT_EQ(controller.layout_store_object(), nullptr);
  controller.set_layout_store(&store);
  EXPECT_EQ(controller.layout_store_object(), &store);
  EXPECT_FALSE(controller.has_snapshot());
  controller.set_layout_store(nullptr);
  EXPECT_EQ(controller.layout_store_object(), nullptr);
}

TEST(EditorNodeController, PublishDocumentSelectsPrimaryColorGrade) {
  EditorNodeController controller;
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 4));
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(controller.backbone_node_ids().size(), 3);
  EXPECT_FALSE(controller.can_add_color_grade());
}

TEST(EditorNodeController, TopologyEditDoesNotSelectASubstituteGrade) {
  DocumentSessionBackend backend;
  backend.SetGeneration(17);
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  controller.selectNode(QStringLiteral("grade.extra"));
  ASSERT_EQ(controller.selected_node_id(), NodeId{"grade.extra"});

  ASSERT_TRUE(controller.deleteColorGrade(QStringLiteral("grade.extra")));
  EXPECT_TRUE(controller.selected_node_id().Empty());
  EXPECT_TRUE(backend.last_projection_node().Empty());
  EXPECT_FALSE(session.submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.3f}, false));
}

TEST(EditorNodeController, UnknownNodeDoesNotCreateASecondSelection) {
  EditorNodeController controller;
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 1));
  controller.selectNode(QStringLiteral("grade.primary"));
  controller.selectNode(QStringLiteral("missing"));
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  EXPECT_FALSE(controller.last_error().isEmpty());
}

TEST(EditorNodeController, BackboneKeysMoveSelectionAlongTheImageBackbone) {
  EditorNodeController controller;
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 1));
  controller.selectDevelop();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"develop"});
  controller.selectNextBackboneNode();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  controller.selectNextBackboneNode();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"drt"});
  controller.selectNextBackboneNode();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"drt"});
  controller.selectPreviousBackboneNode();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  controller.selectDrt();
  EXPECT_EQ(controller.selected_node_id(), NodeId{"drt"});
}

TEST(EditorNodeController, StaleGenerationSnapshotIsRejected) {
  DocumentSessionBackend backend;
  backend.SetGeneration(12);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.has_snapshot());
  EXPECT_EQ(controller.session_generation(), 12u);

  auto stale = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 11, 1, 1);
  EXPECT_FALSE(controller.PublishSnapshot(stale));
  EXPECT_EQ(controller.session_generation(), 12u);
  EXPECT_FALSE(controller.last_error().isEmpty());
}

TEST(EditorNodeController, AddCreatesDisconnectedDraftGradeWithoutAProductCommand) {
  DocumentSessionBackend backend;
  backend.SetGeneration(15);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  controller.selectNode(QStringLiteral("grade.primary"));

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_EQ(backend.Document().NextColorGradeNameNumber(), 2u);
  EXPECT_TRUE(controller.incomplete_draft());
  EXPECT_EQ(controller.backbone_node_ids().size(), 4);
  EXPECT_EQ(controller.selected_node_name(), QStringLiteral("Color Grade 2"));
  EXPECT_EQ(controller.ActiveEdges().size(), 2u);
}

TEST(EditorNodeController, AddPlacesTheDisconnectedGradeBelowTheBackbone) {
  DocumentSessionBackend backend;
  backend.SetGeneration(15);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  EditorNodeLayoutStore   store;
  controller.set_editor_session(&session);
  controller.set_layout_store(&store);
  store.EnsureDefaultPositions(controller.snapshot());

  const auto drt_before = store.NodePosition(NodeId{"drt"});
  ASSERT_TRUE(drt_before.has_value());
  ASSERT_TRUE(controller.addCleanColorGrade());

  const auto staged = store.NodePosition(controller.selected_node_id());
  ASSERT_TRUE(staged.has_value());
  EXPECT_DOUBLE_EQ(staged->x(), drt_before->x());
  EXPECT_GT(staged->y(), drt_before->y());
  EXPECT_EQ(store.NodePosition(NodeId{"drt"}), drt_before);
}

TEST(EditorNodeController, RenameChangesLabelWithoutChangingSelectionIdentity) {
  DocumentSessionBackend backend;
  backend.SetGeneration(16);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);

  ASSERT_TRUE(controller.renameColorGrade(QStringLiteral("grade.primary"), QStringLiteral("Sky")));
  EXPECT_EQ(backend.rename_count(), 1);
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(controller.selected_node_name(), QStringLiteral("Sky"));
}

TEST(EditorNodeController, DeleteOfADraftGradeDoesNotSubmitWhileThePathIsBroken) {
  DocumentSessionBackend backend;
  backend.SetGeneration(17);
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  controller.selectNode(QStringLiteral("grade.primary"));

  ASSERT_TRUE(controller.deleteColorGrade(QStringLiteral("grade.primary")));
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_TRUE(controller.incomplete_draft());
  EXPECT_EQ(controller.ActiveNodes().size(), 3u);
  EXPECT_TRUE(controller.selected_node_id().Empty());
}

TEST(EditorNodeController, EndpointsAndStaleGenerationRejectCommandsBeforeBackendMutation) {
  DocumentSessionBackend backend;
  backend.SetGeneration(18);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);

  EXPECT_FALSE(controller.renameColorGrade(QStringLiteral("develop"), QStringLiteral("Raw")));
  EXPECT_FALSE(controller.deleteColorGrade(QStringLiteral("drt")));
  EXPECT_FALSE(
      controller.renameColorGrade(QStringLiteral("grade.missing"), QStringLiteral("Missing")));
  EXPECT_FALSE(controller.deleteColorGrade(QStringLiteral("grade.missing")));
  backend.SetGeneration(19);
  EXPECT_FALSE(controller.addCleanColorGrade());
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_EQ(backend.rename_count(), 0);
}

TEST(EditorNodeController, BackendFailurePreservesDocumentCounterProjectionAndSelection) {
  DocumentSessionBackend backend;
  backend.SetGeneration(19);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  const auto before_ids      = controller.backbone_node_ids();
  const auto before_counter  = backend.Document().NextColorGradeNameNumber();
  const auto before_selected = controller.selected_node_id();

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_EQ(controller.backbone_node_ids().size(), before_ids.size() + 1);
  EXPECT_EQ(backend.Document().NextColorGradeNameNumber(), before_counter);
  EXPECT_NE(controller.selected_node_id(), before_selected);
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
}

TEST(EditorNodeController, MissingDefaultColorGradeAfterRefreshClearsSelection) {
  EditorNodeController controller;
  auto                 document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(controller.PublishDocument(document, 2));
  controller.selectNode(QStringLiteral("grade.primary"));
  auto next = EditorNodeGraphProjection::Build(document, 2, 0, 0);
  next.nodes.erase(
      std::remove_if(next.nodes.begin(), next.nodes.end(),
                     [](const auto& node) { return node.node_id == NodeId{"grade.primary"}; }),
      next.nodes.end());
  ASSERT_TRUE(controller.PublishSnapshot(std::move(next)));
  EXPECT_TRUE(controller.selected_node_id().Empty());
}

TEST(EditorNodeController, BlockedEditAvailabilityDisablesAddWithoutSnapshotChange) {
  DocumentSessionBackend backend;
  backend.SetGeneration(21);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.can_add_color_grade());

  alcedo::EditorActionAvailability blocked;
  backend.PublishAvailability(blocked);
  EXPECT_FALSE(session.can_edit());
  EXPECT_FALSE(controller.can_add_color_grade());
  EXPECT_FALSE(controller.addCleanColorGrade());
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
}

TEST(EditorNodeController, ActiveCommandRejectsNestedAddBeforeDraftMutation) {
  DocumentSessionBackend backend;
  backend.SetGeneration(22);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_EQ(controller.backbone_node_ids().size(), 4);
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
}

TEST(EditorNodeController, ExclusivePortConnectLeavesDetachedGradesWithoutSubmitting) {
  DocumentSessionBackend backend;
  backend.SetGeneration(30);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id();

  ASSERT_TRUE(controller.requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_TRUE(controller.incomplete_draft());
  ASSERT_TRUE(controller.requestConnect(NodeIdToQString(extra), QStringLiteral("drt")));
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_TRUE(controller.incomplete_draft());
}

TEST(EditorNodeController, CompletingThePathSubmitsOneTopologyChange) {
  DocumentSessionBackend backend;
  backend.SetGeneration(31);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id();
  ASSERT_TRUE(controller.requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  ASSERT_TRUE(controller.requestConnect(NodeIdToQString(extra), QStringLiteral("grade.primary")));
  ASSERT_TRUE(controller.requestConnect(QStringLiteral("grade.primary"), QStringLiteral("drt")));
  EXPECT_EQ(backend.edit_node_graph_count(), 1);
  EXPECT_FALSE(controller.incomplete_draft());
  EXPECT_EQ(backend.Document().Graph().FindNode(extra) != nullptr, true);
  EXPECT_EQ(backend.Document().NextColorGradeNameNumber(), 3u);
  EXPECT_EQ(controller.snapshot().nodes.size(), 4u);
  EXPECT_EQ(controller.ActiveNodes().size(), 4u);
}

TEST(EditorNodeController, ProductTopologyCommitKeepsProjectionFailureVisible) {
  DocumentSessionBackend backend;
  backend.SetGeneration(33);
  EditorSessionController    session(&backend);
  EditorNodeController       controller;
  alcedo::ui::AlcedoQanGraph adapter;
  controller.set_editor_session(&session);
  controller.set_graph_adapter(&adapter);

  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id();
  ASSERT_TRUE(controller.requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  ASSERT_TRUE(controller.requestConnect(NodeIdToQString(extra), QStringLiteral("grade.primary")));

  EXPECT_EQ(backend.edit_node_graph_count(), 1);
  EXPECT_NE(backend.Document().Graph().FindNode(extra), nullptr);
  EXPECT_EQ(controller.snapshot().nodes.size(), 4u);
  EXPECT_FALSE(adapter.has_projection());
  EXPECT_EQ(controller.last_error(), QStringLiteral("AlcedoQanGraph has no Qan graph"));

  QCoreApplication::processEvents();
  EXPECT_EQ(controller.last_error(), QStringLiteral("AlcedoQanGraph has no Qan graph"));
  EXPECT_EQ(controller.snapshot().nodes.size(), 4u);
}

TEST(EditorNodeController, DevelopAndDrtRejectUnsupportedPortRoles) {
  DocumentSessionBackend backend;
  backend.SetGeneration(32);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id_string();

  EXPECT_FALSE(controller.requestConnect(extra, QStringLiteral("develop")));
  EXPECT_EQ(controller.last_error(), QStringLiteral("Develop has no incoming image port"));
  EXPECT_FALSE(controller.requestConnectorMove(extra, QStringLiteral("drt"), true));
  EXPECT_EQ(controller.last_error(),
            QStringLiteral("Connections must go from an output port to an input port"));
  EXPECT_FALSE(controller.requestConnect(QStringLiteral("drt"), extra));
  EXPECT_EQ(controller.last_error(), QStringLiteral("DRT/Post has no outgoing image port"));
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
}

TEST(EditorNodeController, SelfConnectAndStaleGenerationLeaveTheDraftUnchanged) {
  DocumentSessionBackend backend;
  backend.SetGeneration(34);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra  = controller.selected_node_id_string();
  const auto before = controller.ActiveEdges().size();

  EXPECT_FALSE(controller.requestConnect(extra, extra));
  EXPECT_EQ(controller.last_error(), QStringLiteral("A node cannot connect to itself"));
  EXPECT_FALSE(controller.requestConnect(QStringLiteral("develop"), extra, 1));
  EXPECT_EQ(controller.last_error(),
            QStringLiteral("The node command is from another editor session"));
  EXPECT_EQ(controller.ActiveEdges().size(), before);
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
}

TEST(EditorNodeController, ReturningADraftToTheBaseExitsEditingWithoutACommit) {
  DocumentSessionBackend backend;
  backend.SetGeneration(35);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id_string();
  ASSERT_TRUE(controller.deleteColorGrade(extra));
  EXPECT_FALSE(controller.incomplete_draft());
  EXPECT_EQ(backend.edit_node_graph_count(), 0);
  EXPECT_EQ(controller.backbone_node_ids().size(), 3);
}

TEST(EditorNodeController, OrdinaryDraftEditsDoNotCopyIntoTheCommittedSnapshot) {
  DocumentSessionBackend backend;
  backend.SetGeneration(36);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  const auto committed_nodes = controller.snapshot().nodes.size();
  const auto committed_edges = controller.snapshot().edges.size();
  const auto applies_before  = controller.completed_projection_apply_count();

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_EQ(controller.snapshot().nodes.size(), committed_nodes);
  EXPECT_EQ(controller.snapshot().edges.size(), committed_edges);
  EXPECT_EQ(controller.ActiveNodes().size(), committed_nodes + 1);
  EXPECT_EQ(controller.completed_projection_apply_count(), applies_before);
}

TEST(EditorNodeController, IdenticalCommittedProjectionDoesNotPublishOrQueueAnotherApply) {
  EditorNodeController controller;
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 1));
  const auto queued_after_first   = controller.queued_projection_apply_count();
  const auto revision_after_first = controller.projection_revision();
  EXPECT_GE(queued_after_first, 1);
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 1));
  EXPECT_EQ(controller.queued_projection_apply_count(), queued_after_first);
  EXPECT_EQ(controller.projection_revision(), revision_after_first);
  QCoreApplication::processEvents();
  EXPECT_EQ(controller.completed_projection_apply_count(), 0);
  EXPECT_GE(controller.skipped_stale_projection_apply_count(), 1);
}

TEST(EditorNodeController, RepeatedSessionStateChangesDoNotReadHistoryOrRepublishNodes) {
  DocumentSessionBackend backend;
  backend.SetGeneration(81);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  const auto revision             = controller.projection_revision();
  const auto queued               = controller.queued_projection_apply_count();
  const auto active_version_reads = backend.active_version_read_count();

  for (int index = 0; index < 20; ++index) {
    backend.PublishStateChange();
  }

  EXPECT_EQ(backend.history_snapshot_read_count(), 0);
  EXPECT_EQ(backend.active_version_read_count(), active_version_reads);
  EXPECT_EQ(controller.projection_revision(), revision);
  EXPECT_EQ(controller.queued_projection_apply_count(), queued);
}

TEST(EditorNodeController, UnchangedHistoryProjectionDoesNotPublishOrQueueAnotherApply) {
  DocumentSessionBackend backend;
  backend.SetGeneration(82);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  const auto revision = controller.projection_revision();
  const auto queued   = controller.queued_projection_apply_count();

  backend.PublishHistoryChange();

  EXPECT_EQ(backend.history_snapshot_read_count(), 0);
  EXPECT_EQ(controller.projection_revision(), revision);
  EXPECT_EQ(controller.queued_projection_apply_count(), queued);
}

TEST(EditorNodeController, ChangedHistoryProjectionPublishesExactlyOneApply) {
  DocumentSessionBackend backend;
  backend.SetGeneration(83);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  const auto revision = controller.projection_revision();
  const auto queued   = controller.queued_projection_apply_count();
  ASSERT_TRUE(
      alcedo::RenameColorGrade(backend.Document(), NodeId{"grade.primary"}, "Updated").empty());

  backend.PublishHistoryChange();

  EXPECT_EQ(backend.history_snapshot_read_count(), 0);
  EXPECT_EQ(controller.projection_revision(), revision + 1);
  EXPECT_EQ(controller.queued_projection_apply_count(), queued + 1);
}

TEST(EditorNodeController, QueuedProjectionApplyIgnoresStaleAdapterAfterDetach) {
  EditorNodeController controller;
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 2));
  controller.set_graph_adapter(nullptr);
  QCoreApplication::processEvents();
  EXPECT_EQ(controller.completed_projection_apply_count(), 0);
  EXPECT_GE(controller.skipped_stale_projection_apply_count(), 1);
}

TEST(EditorNodeController, LayoutKeyActivatesBeforeSavedSelectionRestore) {
  EditorNodeLayoutStore store;
  EditorNodeController  controller;
  controller.set_layout_store(&store);
  controller.SetLayoutIdentity(1, 2, QStringLiteral("version-a"));
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 3));
  controller.selectNode(QStringLiteral("develop"));
  store.SetNodePosition(NodeId{"grade.primary"}, QPointF(11, 22));
  EXPECT_EQ(store.selected_node_id(), NodeId{"develop"});

  controller.SetLayoutIdentity(1, 2, QStringLiteral("version-b"));
  EXPECT_EQ(store.current_key().version_id, QStringLiteral("version-b"));
  EXPECT_TRUE(store.selected_node_id().Empty());
  EXPECT_FALSE(store.hasNodePosition(QStringLiteral("grade.primary")));

  controller.SetLayoutIdentity(1, 2, QStringLiteral("version-a"));
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 3));
  EXPECT_EQ(store.current_key().version_id, QStringLiteral("version-a"));
  EXPECT_EQ(controller.selected_node_id(), NodeId{"develop"});
  EXPECT_EQ(store.NodePosition(NodeId{"grade.primary"}), QPointF(11, 22));
}

TEST(EditorNodeController, SavedLayoutSelectionDoesNotOverrideLiveSelectionOnTheSameKey) {
  EditorNodeLayoutStore store;
  EditorNodeController  controller;
  controller.set_layout_store(&store);
  controller.SetLayoutIdentity(4, 5, QStringLiteral("v"));
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 4));
  controller.selectNode(QStringLiteral("develop"));
  store.set_selected_node_id(NodeId{"grade.primary"});
  ASSERT_TRUE(controller.PublishDocument(CreateDefaultPipelineDocument(), 4));
  EXPECT_EQ(controller.selected_node_id(), NodeId{"develop"});
}

TEST(EditorNodeController, ImageSwitchAfterSubmitRefreshesTheCommittedProjection) {
  DocumentSessionBackend backend;
  backend.SetGeneration(37);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  EXPECT_EQ(controller.image_id(), 9u);
  backend.SetImageId(50);
  EXPECT_EQ(session.image_id(), 50u);
  EXPECT_EQ(controller.image_id(), 50u);

  backend.SetImageId(9);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id();
  ASSERT_TRUE(controller.requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  ASSERT_TRUE(controller.requestConnect(NodeIdToQString(extra), QStringLiteral("grade.primary")));
  ASSERT_TRUE(controller.requestConnect(QStringLiteral("grade.primary"), QStringLiteral("drt")));
  EXPECT_FALSE(controller.incomplete_draft());
  EXPECT_EQ(controller.image_id(), 9u);

  backend.SetImageId(99);
  EXPECT_EQ(session.image_id(), 99u);
  EXPECT_EQ(controller.image_id(), 99u);
  EXPECT_EQ(controller.snapshot().nodes.size(), 4u);
}

TEST(EditorNodeController, LoadingSessionClearsThePriorGraphAndInteractiveRepublishesIt) {
  DocumentSessionBackend backend;
  backend.SetGeneration(41);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.has_snapshot());
  EXPECT_EQ(controller.snapshot().nodes.size(), 3u);

  backend.SetState(EditorSessionState::Loading);
  EXPECT_FALSE(controller.has_snapshot());
  EXPECT_TRUE(controller.snapshot().nodes.empty());
  EXPECT_EQ(backend.pipeline_document()->Graph().NodeCount(), 3u);

  backend.SetState(EditorSessionState::Switching);
  EXPECT_FALSE(controller.has_snapshot());

  backend.SetState(EditorSessionState::Interactive);
  EXPECT_TRUE(controller.has_snapshot());
  EXPECT_EQ(controller.snapshot().nodes.size(), 3u);
}

TEST(EditorNodeController, KnownDraftIssuesArePresentedAndUnknownFailuresKeepExactText) {
  class SelfConnectTranslator final : public QTranslator {
   public:
    bool isEmpty() const override { return false; }

    QString translate(const char* context, const char* source, const char*, int) const override {
      if (QLatin1String(context) != QLatin1String("EditorNodeGraphPresentation")) {
        return {};
      }
      if (QLatin1String(source) == QLatin1String("A node cannot connect to itself")) {
        return QStringLiteral("Ein Knoten kann nicht mit sich selbst verbunden werden");
      }
      return {};
    }
  };

  EXPECT_EQ(PresentNodeGraphDraftIssue(alcedo::NodeGraphDraftIssue::None,
                                       "mini-Git journal append failed"),
            QStringLiteral("mini-Git journal append failed"));
  EXPECT_EQ(PresentNodeGraphDraftIssue(alcedo::NodeGraphDraftIssue::SelfConnection, {}),
            QStringLiteral("A node cannot connect to itself"));

  SelfConnectTranslator translator;
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
  EXPECT_EQ(PresentNodeGraphDraftIssue(alcedo::NodeGraphDraftIssue::SelfConnection, {}),
            QStringLiteral("Ein Knoten kann nicht mit sich selbst verbunden werden"));
  EXPECT_EQ(PresentNodeGraphDraftIssue(alcedo::NodeGraphDraftIssue::None,
                                       "mini-Git journal append failed"),
            QStringLiteral("mini-Git journal append failed"));

  DocumentSessionBackend backend;
  backend.SetGeneration(42);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  ASSERT_TRUE(controller.addCleanColorGrade());
  const auto extra = controller.selected_node_id_string();
  EXPECT_FALSE(controller.requestConnect(extra, extra));
  EXPECT_EQ(controller.last_error(),
            QStringLiteral("Ein Knoten kann nicht mit sich selbst verbunden werden"));
  QCoreApplication::removeTranslator(&translator);
}

TEST(EditorSessionToolPanelPage, AcceptsOnlyEmptyHistoryVersionsAndNodes) {
  EditorSessionController session;
  session.set_editor_tool_panel_page(QStringLiteral("history"));
  EXPECT_EQ(session.editor_tool_panel_page(), QStringLiteral("history"));
  session.set_editor_tool_panel_page(QStringLiteral("versions"));
  EXPECT_EQ(session.editor_tool_panel_page(), QStringLiteral("versions"));
  session.set_editor_tool_panel_page(QStringLiteral("nodes"));
  EXPECT_EQ(session.editor_tool_panel_page(), QStringLiteral("nodes"));
  session.set_editor_tool_panel_page(QStringLiteral("tone"));
  EXPECT_TRUE(session.editor_tool_panel_page().isEmpty());
  session.set_editor_tool_panel_page(QStringLiteral("NODES"));
  EXPECT_EQ(session.editor_tool_panel_page(), QStringLiteral("nodes"));
}

TEST(EditorNodeController, SubmitWriteStampsSelectedColorGradeInstance) {
  DocumentSessionBackend  backend;
  ASSERT_TRUE(AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EditorSessionController session(&backend);
  EditorNodeController    nodes;
  nodes.set_editor_session(&session);
  nodes.selectNode(NodeIdToQString(NodeId{"grade.b"}));
  EXPECT_EQ(backend.last_projection_node(), NodeId{"grade.b"});

  ASSERT_TRUE(session.submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.4f}, false));
  const auto pending = session.PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().captured_target.owner_kind,
            EditorParameterOwnerKind::ColorGrade);
  EXPECT_EQ(pending.sequences.front().captured_target.node_id, NodeId{"grade.b"});
  const auto* extra =
      dynamic_cast<alcedo::ColorGradeNodeModel*>(backend.Document().Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(extra, nullptr);
  const auto* instance = extra->FindAdjustmentIdByType(alcedo::type_ids::Exposure());
  ASSERT_NE(instance, nullptr);
  EXPECT_EQ(pending.sequences.front().captured_target.adjustment_instance_id, *instance);
  EXPECT_NE(pending.sequences.front().captured_target.adjustment_instance_id,
            alcedo::AdjustmentInstanceId{"grade.primary.exposure"});
}

TEST(EditorNodeController, GeometryWriteRejectedWhenColorGradeIsSelected) {
  DocumentSessionBackend  backend;
  EditorSessionController session(&backend);
  EditorNodeController    nodes;
  nodes.set_editor_session(&session);
  ASSERT_TRUE(session.submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.1f}, true));
  EXPECT_FALSE(session.submitWrite(QStringLiteral("crop_rotate"), ImageGeometryUpdate{}, false));
  const auto pending = session.PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 1u);
  EXPECT_EQ(pending.sequences.front().fields.size(), 1u);
  EXPECT_EQ(pending.sequences.front().fields.front().target.field_key, "exposure");
}

TEST(EditorNodeController, EmptySelectionDoesNotQueueBoundaryOrRender) {
  DocumentSessionBackend  backend;
  ASSERT_TRUE(AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EditorSessionController session(&backend);
  EditorNodeController    nodes;
  nodes.set_editor_session(&session);
  const int views_before      = backend.view_change_count();
  const int boundaries_before = backend.boundary_count();
  const int enqueue_before    = backend.enqueue_count();
  const auto revision_before  = backend.history_revision();
  nodes.selectNode(NodeIdToQString(NodeId{"grade.b"}));
  EXPECT_EQ(backend.boundary_count(), boundaries_before);
  EXPECT_EQ(backend.enqueue_count(), enqueue_before);
  EXPECT_EQ(backend.view_change_count(), views_before);
  EXPECT_EQ(backend.history_revision(), revision_before);
  EXPECT_EQ(backend.last_projection_node(), NodeId{"grade.b"});
  EXPECT_TRUE(session.PeekPendingInput().sequences.empty());
}

TEST(EditorNodeController, NodeSwitchSealsOpenSequenceAndLaterWriteTargetsNewGrade) {
  DocumentSessionBackend  backend;
  ASSERT_TRUE(AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.b"}).empty());
  EditorSessionController session(&backend);
  EditorNodeController    nodes;
  nodes.set_editor_session(&session);
  ASSERT_EQ(nodes.selected_node_id(), NodeId{"grade.primary"});
  ASSERT_TRUE(session.submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.1f}, false));
  nodes.selectNode(NodeIdToQString(NodeId{"grade.b"}));
  EXPECT_EQ(backend.last_boundary(), EditorPendingInputBoundaryKind::NodeSwitch);
  ASSERT_TRUE(session.submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.2f}, false));

  const auto pending = session.PeekPendingInput();
  ASSERT_EQ(pending.sequences.size(), 2u);
  EXPECT_EQ(pending.sequences[0].seal, EditorPendingInputBoundaryKind::NodeSwitch);
  EXPECT_EQ(pending.sequences[0].captured_target.node_id, NodeId{"grade.primary"});
  EXPECT_EQ(pending.sequences[1].captured_target.node_id, NodeId{"grade.b"});
}

TEST(EditorNodeController, LeavingDevelopGeometryDoesNotRequestViewChange) {
  DocumentSessionBackend  backend;
  EditorSessionController session(&backend);
  EditorNodeController    nodes;
  nodes.set_editor_session(&session);
  nodes.selectNode(NodeIdToQString(NodeId{"develop"}));
  session.set_active_adjustment_panel(QStringLiteral("geometry"));
  const int views_after_geometry = backend.view_change_count();
  EXPECT_GT(views_after_geometry, 0);
  nodes.selectNode(NodeIdToQString(NodeId{"grade.primary"}));
  EXPECT_EQ(session.active_adjustment_panel(), QStringLiteral("tone"));
  EXPECT_EQ(backend.view_change_count(), views_after_geometry);
}

}  // namespace
