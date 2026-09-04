//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QMetaObject>
#include <QPointF>
#include <QQuickItem>
#include <QuickQanava>

#include "editor_history_versions_rail_qml_harness.hpp"
#include "qanGraph.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_node_layout_store.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace alcedo::ui::test {
namespace {

using rail_harness::Click;
using rail_harness::ProcessEvents;
using rail_harness::RailQmlFixture;

class EditorNodesPanelQmlTest : public RailQmlFixture {
 protected:
  void SetUp() override { RailQmlFixture::SetUp(); }

  auto Controller() -> EditorNodeController* {
    return window_ == nullptr ? nullptr : window_->findChild<EditorNodeController*>();
  }

  auto LayoutStore() -> EditorNodeLayoutStore* {
    return window_ == nullptr ? nullptr : window_->findChild<EditorNodeLayoutStore*>();
  }

  auto Adapter() -> AlcedoQanGraph* {
    return window_ == nullptr ? nullptr : window_->findChild<AlcedoQanGraph*>();
  }
};

TEST_F(EditorNodesPanelQmlTest, HistoryVersionsAndNodesAreMutuallyExclusive) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) != nullptr, 2000);
  EXPECT_EQ(controller_.editor_tool_panel_page(), QStringLiteral("history"));

  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) == nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsPageBody")) == nullptr, 2000);
  EXPECT_EQ(controller_.editor_tool_panel_page(), QStringLiteral("nodes"));

  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsPageBody")) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) == nullptr, 2000);
}

TEST_F(EditorNodesPanelQmlTest, GraphCanvasPaintsUniformBackgroundWithoutGrid) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesGraphView")) != nullptr, 2000);
  auto* view = Find(QStringLiteral("editorNodesGraphView"));
  auto* grid = view->property("grid").value<QObject*>();
  ASSERT_NE(grid, nullptr) << "grid: null swaps in QuickQanava's empty default grid";
  EXPECT_STREQ(grid->metaObject()->className(), "qan::Grid")
      << "the painted Qan.LineGrid must not be installed on the Nodes canvas";
}

TEST_F(EditorNodesPanelQmlTest, FullCloseDestroysGraphDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("qan::NodeItem")) != nullptr, 2000);
  auto* rail = Find(QStringLiteral("editorWorkspaceRail"));
  ASSERT_NE(rail, nullptr);
  const int destroys_before = rail->property("panelBodyDestroyCount").toInt();

  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) == nullptr, 2000);
  EXPECT_EQ(Find(QStringLiteral("qan::NodeItem")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesQanGraph")), nullptr);
  EXPECT_GE(rail->property("panelBodyDestroyCount").toInt(), destroys_before + 1);
}

TEST_F(EditorNodesPanelQmlTest, ReopenRestoresPositionsViewZoomSelectionAndDrawerState) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* adapter = Adapter();
  auto* store   = LayoutStore();
  auto* nodes   = Controller();
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);

  const NodeId grade{"grade.primary"};
  adapter->SetNodeItemPosition(grade, QPointF(120, 160));
  store->SetNodePosition(grade, QPointF(120, 160));
  adapter->SetDrawerOpen(grade, false);
  store->SetDrawerOpen(grade, false);
  auto* graph_view = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(graph_view, nullptr);
  ASSERT_TRUE(graph_view->setProperty("zoom", 1.25));
  auto* container = graph_view->property("containerItem").value<QQuickItem*>();
  ASSERT_NE(container, nullptr);
  container->setX(-12);
  container->setY(-18);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(Find(QStringLiteral("editorNodesPageBody")), "captureView"));
  nodes->selectNode(QStringLiteral("develop"));

  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) == nullptr, 2000);
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  adapter = Adapter();
  store   = LayoutStore();
  nodes   = Controller();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->has_projection(), 2000);
  EXPECT_EQ(adapter->NodeItemPosition(grade), QPointF(120, 160));
  EXPECT_FALSE(adapter->DrawerOpen(grade));
  EXPECT_DOUBLE_EQ(store->zoom(), 1.25);
  EXPECT_EQ(store->view_position(), QPointF(-12, -18));
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"develop"});
}

TEST_F(EditorNodesPanelQmlTest, TwoVersionsKeepSeparateLayoutValues) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(LayoutStore() != nullptr, 2000);
  auto* store = LayoutStore();
  auto* nodes = Controller();
  ASSERT_NE(store, nullptr);
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);

  const QString first_version = nodes->version_id();
  store->SetNodePosition(NodeId{"grade.primary"}, QPointF(15, 25));
  store->SetDrawerOpen(NodeId{"grade.primary"}, false);

  backend_.CheckoutVersion(rail_harness::StableId(2));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(nodes->version_id() != first_version, 2000);
  EXPECT_NE(store->NodePosition(NodeId{"grade.primary"}), QPointF(15, 25));
  EXPECT_TRUE(store->DrawerOpen(NodeId{"grade.primary"}));
  store->SetNodePosition(NodeId{"grade.primary"}, QPointF(70, 80));

  backend_.CheckoutVersion(rail_harness::StableId(1));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(nodes->version_id() == first_version, 2000);
  EXPECT_EQ(store->NodePosition(NodeId{"grade.primary"}), QPointF(15, 25));
  EXPECT_FALSE(store->DrawerOpen(NodeId{"grade.primary"}));
}

TEST_F(EditorNodesPanelQmlTest, CtrlClickCannotCreateASecondProductSelection) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* adapter = Adapter();
  auto* nodes   = Controller();
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"develop"}) != nullptr, 2000);

  auto* develop = adapter->NodeFor(NodeId{"develop"});
  auto* grade   = adapter->NodeFor(NodeId{"grade.primary"});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(grade, nullptr);
  ASSERT_NE(develop->getItem(), nullptr);
  ASSERT_NE(grade->getItem(), nullptr);

  QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                    develop->getItem()->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();
  QTest::mouseClick(window_, Qt::LeftButton, Qt::ControlModifier,
                    grade->getItem()->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  ASSERT_NE(adapter->graph(), nullptr);
  EXPECT_FALSE(adapter->graph()->hasMultipleSelection());
}

TEST_F(EditorNodesPanelQmlTest, GraphMovementAndDrawerFoldsDoNotStartPhotoRendering) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  const int  patches_before = backend_.patch_count();
  const int  views_before   = backend_.view_change_count();
  const auto history_before = backend_.history_revision();

  auto*      adapter        = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  adapter->SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(40, 90));
  adapter->SetDrawerOpen(NodeId{"grade.primary"}, false);
  ProcessEvents();

  auto* graph_view = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(graph_view, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(Find(QStringLiteral("editorNodesPageBody")), "fitGraph"));
  ProcessEvents();

  EXPECT_EQ(backend_.patch_count(), patches_before);
  EXPECT_EQ(backend_.view_change_count(), views_before);
  EXPECT_EQ(backend_.history_revision(), history_before);
}

TEST_F(EditorNodesPanelQmlTest, ReduceMotionMakesRelatedFoldsImmediate) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  EXPECT_TRUE(AppTheme::Instance().reduceMotion());
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodeMaskDrawer")) != nullptr, 2000);
  auto* drawer = Find(QStringLiteral("editorNodeMaskDrawer"));
  ASSERT_NE(drawer, nullptr);
  EXPECT_TRUE(drawer->property("expanded").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(drawer, "toggle"));
  ProcessEvents();
  EXPECT_FALSE(drawer->property("expanded").toBool());
  EXPECT_NEAR(drawer->property("foldProgress").toReal(), 0.0, 0.001);
}

TEST_F(EditorNodesPanelQmlTest, AddActionDoesNotMutateTheGraph) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesAddButton")) != nullptr, 2000);
  auto* add = Find(QStringLiteral("editorNodesAddButton"));
  ASSERT_NE(add, nullptr);
  EXPECT_FALSE(add->property("enabled").toBool());
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("Add Color Grade"));
  const auto history_before = backend_.history_revision();
  Click(window_, add);
  EXPECT_EQ(backend_.history_revision(), history_before);
}

}  // namespace
}  // namespace alcedo::ui::test
