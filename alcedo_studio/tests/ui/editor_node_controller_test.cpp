//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "app/editor_action_policy.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "grade_owned_mask_support.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace {

using alcedo::CreateDefaultPipelineDocument;
using alcedo::EditorNodeGraphProjection;
using alcedo::EditorSessionIdentity;
using alcedo::EditorSessionResult;
using alcedo::EditorSessionState;
using alcedo::IEditorSessionBackend;
using alcedo::NodeId;
using alcedo::PipelineDocument;
using alcedo::ui::EditorNodeController;
using alcedo::ui::EditorNodeLayoutStore;
using alcedo::ui::EditorSessionController;

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

  [[nodiscard]] auto state() const -> EditorSessionState override {
    return EditorSessionState::Interactive;
  }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override { return true; }
  [[nodiscard]] auto has_image() const -> bool override { return true; }
  [[nodiscard]] auto last_error() const -> std::string override { return {}; }
  [[nodiscard]] auto pipeline_document() const -> const PipelineDocument* override {
    return &document_;
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
  auto Open(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Switch(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Close(bool) -> EditorSessionResult override { return {}; }
  auto Shutdown() -> EditorSessionResult override { return {}; }
  auto Discard() -> EditorSessionResult override { return {}; }
  auto Undo() -> EditorSessionResult override { return {}; }
  auto Redo() -> EditorSessionResult override { return {}; }

  auto AddColorGrade(const NodeId& before_node_id, const NodeId& new_id)
      -> EditorSessionResult override {
    if (before_add_) {
      before_add_();
    }
    ++add_count_;
    if (fail_commands_) return Rejected("mini-Git journal append failed");
    const auto errors = alcedo::AddCleanColorGrade(document_, before_node_id, new_id);
    if (!errors.empty()) return Rejected(errors.front().message);
    NotifyChange();
    return Accepted("Color Grade created");
  }
  auto RemoveColorGrade(const NodeId& node_id) -> EditorSessionResult override {
    ++remove_count_;
    if (fail_commands_) return Rejected("mini-Git journal append failed");
    const auto errors = alcedo::RemoveColorGradeAndBridge(document_, node_id);
    if (!errors.empty()) return Rejected(errors.front().message);
    NotifyChange();
    return Accepted("Color Grade removed");
  }
  auto RenameColorGrade(const NodeId& node_id, std::string display_name)
      -> EditorSessionResult override {
    ++rename_count_;
    if (fail_commands_) return Rejected("mini-Git journal append failed");
    const auto errors = alcedo::RenameColorGrade(document_, node_id, std::move(display_name));
    if (!errors.empty()) return Rejected(errors.front().message);
    NotifyChange();
    return Accepted("Color Grade renamed");
  }

  void SetGeneration(std::uint64_t value) { request_.value = value; }
  auto Document() -> PipelineDocument& { return document_; }
  void SetFailCommands(bool fail) { fail_commands_ = fail; }
  void PublishAvailability(alcedo::EditorActionAvailability availability) {
    availability_ = std::move(availability);
    if (availability_observer_) {
      availability_observer_(availability_);
    }
  }
  void SetBeforeAdd(std::function<void()> before_add) { before_add_ = std::move(before_add); }
  [[nodiscard]] auto add_count() const -> int { return add_count_; }
  [[nodiscard]] auto remove_count() const -> int { return remove_count_; }
  [[nodiscard]] auto rename_count() const -> int { return rename_count_; }

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
  std::function<void()>                             before_add_;
  bool                                              fail_commands_ = false;
  int                                               add_count_     = 0;
  int                                               remove_count_  = 0;
  int                                               rename_count_  = 0;
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

TEST(EditorNodeController, AddCreatesCleanGradeAfterSelectedGradeAndConsumesOneName) {
  DocumentSessionBackend backend;
  backend.SetGeneration(15);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  controller.selectNode(QStringLiteral("grade.primary"));

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_EQ(backend.add_count(), 1);
  EXPECT_EQ(backend.Document().NextColorGradeNameNumber(), 3u);
  const auto& ids = controller.backbone_node_ids();
  ASSERT_EQ(ids.size(), 4);
  EXPECT_EQ(ids[0], QStringLiteral("develop"));
  EXPECT_EQ(ids[1], QStringLiteral("grade.primary"));
  EXPECT_EQ(ids[2], controller.selected_node_id_string());
  EXPECT_EQ(ids[3], QStringLiteral("drt"));
  EXPECT_EQ(controller.selected_node_name(), QStringLiteral("Color Grade 2"));
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

TEST(EditorNodeController, DeleteSelectsSuccessorThenUndoProjectionRestoresDeletedSelection) {
  DocumentSessionBackend backend;
  backend.SetGeneration(17);
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(backend.Document(), NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  auto* primary = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      backend.Document().Graph().FindNode(NodeId{"grade.primary"}));
  ASSERT_NE(primary, nullptr);
  primary->AddMask(alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}), 0);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  controller.selectNode(QStringLiteral("grade.primary"));
  const auto primary_masks = [&controller] {
    for (const auto& node : controller.snapshot().nodes) {
      if (node.node_id == NodeId{"grade.primary"}) {
        return node.masks;
      }
    }
    return std::vector<alcedo::EditorNodeMaskProjection>{};
  };
  ASSERT_EQ(primary_masks().size(), 1u);

  ASSERT_TRUE(controller.deleteColorGrade(QStringLiteral("grade.primary")));
  EXPECT_EQ(backend.remove_count(), 1);
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.extra"});

  auto undo_document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(undo_document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  auto* restored = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      undo_document.Graph().FindNode(NodeId{"grade.primary"}));
  ASSERT_NE(restored, nullptr);
  restored->AddMask(alcedo::grade_mask_test::MakeRadialMask(alcedo::MaskId{"mask.radial"}), 0);
  ASSERT_TRUE(controller.PublishDocument(undo_document, 17));
  EXPECT_EQ(controller.selected_node_id(), NodeId{"grade.primary"});
  ASSERT_EQ(primary_masks().size(), 1u);
  EXPECT_EQ(primary_masks().front().mask_id, alcedo::MaskId{"mask.radial"});
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
  EXPECT_EQ(backend.add_count(), 0);
  EXPECT_EQ(backend.remove_count(), 0);
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
  backend.SetFailCommands(true);

  EXPECT_FALSE(controller.addCleanColorGrade());
  EXPECT_EQ(controller.backbone_node_ids(), before_ids);
  EXPECT_EQ(backend.Document().NextColorGradeNameNumber(), before_counter);
  EXPECT_EQ(controller.selected_node_id(), before_selected);
  EXPECT_EQ(controller.last_error(), QStringLiteral("mini-Git journal append failed"));
}

TEST(EditorNodeController, MissingNodeAfterRefreshSelectsRemainingColorGrade) {
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
  EXPECT_NE(controller.selected_node_id(), NodeId{"grade.primary"});
  EXPECT_FALSE(controller.selected_node_id().Empty());
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
  EXPECT_EQ(backend.add_count(), 0);
}

TEST(EditorNodeController, ActiveCommandRejectsNestedAddBeforeBackendMutation) {
  DocumentSessionBackend backend;
  backend.SetGeneration(22);
  EditorSessionController session(&backend);
  EditorNodeController    controller;
  controller.set_editor_session(&session);
  bool nested_accepted = true;
  backend.SetBeforeAdd([&] { nested_accepted = controller.addCleanColorGrade(); });

  ASSERT_TRUE(controller.addCleanColorGrade());
  EXPECT_FALSE(nested_accepted);
  EXPECT_EQ(backend.add_count(), 1);
  EXPECT_EQ(controller.backbone_node_ids().size(), 4);
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

}  // namespace
