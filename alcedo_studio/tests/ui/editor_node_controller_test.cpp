//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "app/editor_node_graph_projection.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "type/type.hpp"
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
using alcedo::ui::EditorSessionController;

class DocumentSessionBackend final : public IEditorSessionBackend {
 public:
  DocumentSessionBackend() {
    identity_.element_id = 8;
    identity_.image_id   = 9;
    document_            = CreateDefaultPipelineDocument();
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

  void SetPresentationSinkId(alcedo::PresentationSinkId) override {}
  void SetPresentationSize(int, int) override {}
  auto Open(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Switch(sl_element_id_t, image_id_t) -> EditorSessionResult override { return {}; }
  auto Close(bool) -> EditorSessionResult override { return {}; }
  auto Shutdown() -> EditorSessionResult override { return {}; }
  auto Discard() -> EditorSessionResult override { return {}; }
  auto Undo() -> EditorSessionResult override { return {}; }
  auto Redo() -> EditorSessionResult override { return {}; }

  void SetGeneration(std::uint64_t value) { request_.value = value; }
  auto Document() -> PipelineDocument& { return document_; }

 private:
  EditorSessionIdentity      identity_{};
  PipelineDocument           document_;
  alcedo::ImageLoadRequestId request_{};
};

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
