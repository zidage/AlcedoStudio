//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QAccessible>
#include <QMetaObject>
#include <QPointF>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTranslator>
#include <QuickQanava>
#include <algorithm>
#include <functional>

#include "app/editor_node_graph_projection.hpp"
#include "editor_history_versions_rail_qml_harness.hpp"
#include "qanConnector.h"
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

auto AttachedName(QObject* object) -> QString {
  if (object == nullptr) {
    return {};
  }
  QQmlProperty property(object, QStringLiteral("Accessible.name"), qmlContext(object));
  const auto   name = property.read().toString();
  if (!name.isEmpty()) {
    return name;
  }
  const auto dotted = object->property("Accessible.name").toString();
  if (!dotted.isEmpty()) {
    return dotted;
  }
  return object->property("actionName").toString();
}

auto AttachedDescription(QObject* object) -> QString {
  if (object == nullptr) {
    return {};
  }
  QQmlProperty property(object, QStringLiteral("Accessible.description"), qmlContext(object));
  const auto   description = property.read().toString();
  if (!description.isEmpty()) {
    return description;
  }
  return object->property("Accessible.description").toString();
}

void CollectAccessiblePhrases(QObject* object, QStringList* phrases) {
  if (object == nullptr || phrases == nullptr) {
    return;
  }
  const auto name = AttachedName(object);
  if (!name.isEmpty()) {
    phrases->push_back(name);
  }
  const auto description = AttachedDescription(object);
  if (!description.isEmpty()) {
    phrases->push_back(description);
  }
  for (auto* child : object->children()) {
    CollectAccessiblePhrases(child, phrases);
  }
}

class NodesPanelTextExpander final : public QTranslator {
 public:
  bool isEmpty() const override { return false; }

  QString translate(const char* /*context*/, const char* source, const char*, int) const override {
    const auto text = QString::fromUtf8(source);
    static const QStringList owned = {
        QStringLiteral("Nodes"),
        QStringLiteral("Add Color Grade"),
        QStringLiteral("Select an image to edit nodes"),
        QStringLiteral("Loading node graph"),
        QStringLiteral("Updating node graph"),
        QStringLiteral("Select a destination node and press Enter"),
        QStringLiteral("Nodes graph"),
        QStringLiteral("Collapse Masks"),
        QStringLiteral("Expand Masks"),
        QStringLiteral("Masks"),
        QStringLiteral("Fit"),
        QStringLiteral("Rename Color Grade"),
        QStringLiteral("Delete Color Grade"),
    };
    if (!owned.contains(text)) {
      return {};
    }
    const int extra = std::max(2, static_cast<int>(text.size()) * 2 / 5);
    return text + QString(extra, QLatin1Char('W'));
  }
};

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

  auto LiveQanNodeItemCount() const -> int {
    if (window_ == nullptr) {
      return 0;
    }
    int count = 0;
    for (auto* object : window_->findChildren<QObject*>()) {
      if (object->objectName() == QLatin1String("qan::NodeItem")) {
        ++count;
      }
    }
    return count;
  }

  void WaitUntilGraphReady() {
    auto* nodes = Controller();
    auto* view  = Find(QStringLiteral("editorNodesGraphView"));
    ASSERT_NE(nodes, nullptr);
    ASSERT_NE(view, nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(view->isVisible() && view->isEnabled(), 2000);
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

TEST_F(EditorNodesPanelQmlTest,
       RepeatedNodesPageOpenAndCloseCyclesReleaseQanItemsAndIgnoreStaleRefreshes) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();

  for (int cycle = 0; cycle < 100; ++cycle) {
    OpenNodesPage();
    QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 3, 2000);

    const QString display_name = QStringLiteral("Cycle %1").arg(cycle);
    const auto    result =
        backend_.RenameColorGrade(NodeId{"grade.primary"}, display_name.toStdString());
    ASSERT_FALSE(alcedo::EditorSessionResultIsFailure(result.kind));
    controller_.set_editor_tool_panel_page(QString());
    ProcessEvents();
    QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 0, 2000);

    OpenNodesPage();
    QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 3, 2000);
    auto* nodes = Controller();
    ASSERT_NE(nodes, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(nodes->selected_node_name(), display_name, 2000);
  }
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

TEST_F(EditorNodesPanelQmlTest, AddActionCreatesAndSelectsOneCleanColorGrade) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesAddButton")) != nullptr, 2000);
  auto* add = Find(QStringLiteral("editorNodesAddButton"));
  ASSERT_NE(add, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(add->property("enabled").toBool(), 2000);
  EXPECT_TRUE(add->property("enabled").toBool());
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("Add Color Grade"));
  const auto history_before = backend_.history_revision();
  Click(window_, add);
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(nodes->backbone_node_ids().size(), 4, 2000);
  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_EQ(backend_.history_revision(), history_before);
  EXPECT_TRUE(nodes->incomplete_draft());
  EXPECT_EQ(nodes->selected_node_name(), QStringLiteral("Color Grade 2"));
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(nodes->selected_node_id()) != nullptr, 2000);
  ASSERT_NE(adapter->graph(), nullptr);
  EXPECT_EQ(adapter->graph()->getNodeCount(), 4);
  EXPECT_EQ(nodes->graph_adapter_object(), adapter);
  EXPECT_NE(Find(QStringLiteral("editorNodesPageBody")), nullptr);
  auto* guidance = Find(QStringLiteral("editorNodesEditingGuidance"));
  ASSERT_NE(guidance, nullptr);
  EXPECT_TRUE(guidance->property("visible").toBool());
}

TEST_F(EditorNodesPanelQmlTest, OpenNodesPageBindsTheLiveQanAdapterOnTheController) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->graph_adapter_object() == adapter, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
}

TEST_F(EditorNodesPanelQmlTest, CtrlPlusAddsTheNextCleanColorGrade) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* view  = Find(QStringLiteral("editorNodesGraphView"));
  auto* nodes = Controller();
  ASSERT_NE(view, nullptr);
  ASSERT_NE(nodes, nullptr);
  WaitUntilGraphReady();
  view->forceActiveFocus();
  ASSERT_TRUE(view->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Plus, Qt::ControlModifier);
  ProcessEvents();

  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_EQ(nodes->selected_node_name(), QStringLiteral("Color Grade 2"));
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(nodes->selected_node_id()) != nullptr, 2000);
  ASSERT_NE(adapter->graph(), nullptr);
  EXPECT_EQ(adapter->graph()->getNodeCount(), 4);
  EXPECT_EQ(nodes->graph_adapter_object(), adapter);
}

TEST_F(EditorNodesPanelQmlTest, F2RenamesSelectedColorGradeAndKeepsItsIdentity) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* view  = Find(QStringLiteral("editorNodesGraphView"));
  auto* nodes = Controller();
  ASSERT_NE(view, nullptr);
  ASSERT_NE(nodes, nullptr);
  WaitUntilGraphReady();
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_F2);
  ProcessEvents();

  auto* field = Find(QStringLiteral("editorNodeRenameField"));
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(field->isVisible());
  field->setProperty("text", QStringLiteral("Sky"));
  field->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();

  EXPECT_EQ(backend_.rename_grade_count(), 1);
  EXPECT_EQ(backend_.last_renamed_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(nodes->selected_node_name(), QStringLiteral("Sky"));
}

TEST_F(EditorNodesPanelQmlTest, NodeContextMenuOffersRenameAndDeleteOnlyForColorGrades) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    grade_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  auto* menu = window_->findChild<QObject*>(QStringLiteral("editorNodesNodeMenu"));
  ASSERT_NE(menu, nullptr);
  EXPECT_TRUE(menu->property("visible").toBool());
  auto* rename = Find(QStringLiteral("editorNodesRenameMenuItem"));
  auto* remove = Find(QStringLiteral("editorNodesDeleteMenuItem"));
  ASSERT_NE(rename, nullptr);
  ASSERT_NE(remove, nullptr);
  EXPECT_TRUE(rename->property("enabled").toBool());
  EXPECT_TRUE(remove->property("enabled").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorNodesEnableMenuItem")), nullptr);

  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  nodes->selectDevelop();
  ProcessEvents();
  EXPECT_FALSE(rename->property("enabled").toBool());
  EXPECT_FALSE(remove->property("enabled").toBool());
}

TEST_F(EditorNodesPanelQmlTest, DeleteKeyRemovesSelectedGradeAndSelectsItsSuccessor) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(view, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->addCleanColorGrade());
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(nodes->selected_node_id()) != nullptr, 2000);
  const auto extra = nodes->selected_node_id();
  nodes->selectNode(QStringLiteral("grade.primary"));
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_Delete);
  ProcessEvents();

  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_TRUE(nodes->incomplete_draft());
  EXPECT_NE(nodes->selected_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(nodes->backbone_node_ids().size(), 3);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) == nullptr, 2000);
  ASSERT_NE(adapter->graph(), nullptr);
  EXPECT_EQ(adapter->graph()->getNodeCount(), 3);
  const NodeId remaining[] = {NodeId{"develop"}, extra, NodeId{"drt"}};
  for (const auto& id : remaining) {
    QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(id) != nullptr, 2000);
    auto* item = adapter->NodeFor(id)->getItem();
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isVisible());
    EXPECT_NE(item->parentItem(), nullptr);
  }
  const auto visible_on_view = [view]() {
    int                              visible = 0;
    std::function<void(QQuickItem*)> walk    = [&](QQuickItem* item) {
      if (item == nullptr) {
        return;
      }
      if (item->objectName() == QLatin1String("qan::NodeItem") && item->isVisible()) {
        ++visible;
      }
      const auto children = item->childItems();
      for (auto* child : children) {
        walk(child);
      }
    };
    walk(view);
    return visible;
  };
  QTRY_COMPARE_WITH_TIMEOUT(visible_on_view(), 3, 2000);
}

TEST_F(EditorNodesPanelQmlTest, VisualConnectorIsRequestOnlyWithThemeCandidateColor) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->graph() != nullptr, 2000);
  auto* graph = adapter->graph();
  QTRY_VERIFY_WITH_TIMEOUT(graph->getConnector() != nullptr, 2000);
  EXPECT_TRUE(graph->getConnectorEnabled());
  EXPECT_FALSE(graph->getConnectorCreateDefaultEdge());
  EXPECT_EQ(graph->getConnectorEdgeColor(), AppTheme::Instance().graphCandidateEdgeColor());
  EXPECT_EQ(graph->getConnectorColor(), AppTheme::Instance().graphPortBorderColor());
}

TEST_F(EditorNodesPanelQmlTest, ConnectorMovePromotesPermanentEdgesFromAcceptedProjection) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->addCleanColorGrade());
  const auto extra = nodes->selected_node_id();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(extra) != nullptr, 2000);
  const auto replace_count = adapter->topology_replace_count();

  ASSERT_TRUE(nodes->requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  ASSERT_TRUE(nodes->requestConnect(NodeIdToQString(extra), QStringLiteral("grade.primary")));
  ASSERT_TRUE(nodes->requestConnect(QStringLiteral("grade.primary"), QStringLiteral("drt")));
  QTRY_COMPARE_WITH_TIMEOUT(backend_.edit_node_graph_count(), 1, 2000);
  EXPECT_EQ(adapter->topology_replace_count(), replace_count);
  EXPECT_FALSE(nodes->incomplete_draft());
  EXPECT_NE(adapter->EdgeFor(EditorNodeEdgeProjection{NodeId{"develop"}, PortId{"image"}, extra,
                                                      PortId{"image"}}),
            nullptr);
}

TEST_F(EditorNodesPanelQmlTest, DrawerFoldDoesNotChangeReconnectNeighbors) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* output = adapter->OutputPortFor(NodeId{"grade.primary"}, PortId{"image"});
  auto* input  = adapter->InputPortFor(NodeId{"grade.primary"}, PortId{"image"});
  ASSERT_NE(output, nullptr);
  ASSERT_NE(input, nullptr);
  adapter->SetDrawerOpen(NodeId{"grade.primary"}, false);
  ProcessEvents();
  EXPECT_EQ(adapter->OutputPortFor(NodeId{"grade.primary"}, PortId{"image"}), output);
  EXPECT_EQ(adapter->InputPortFor(NodeId{"grade.primary"}, PortId{"image"}), input);
}

TEST_F(EditorNodesPanelQmlTest, FailedReconnectKeepsPermanentEdgesAndShowsExactError) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->addCleanColorGrade());
  const auto extra = nodes->selected_node_id();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(extra) != nullptr, 2000);
  ASSERT_TRUE(nodes->requestConnect(QStringLiteral("develop"), NodeIdToQString(extra)));
  backend_.SetFailNodeCommands(true);
  EXPECT_FALSE(nodes->requestConnect(NodeIdToQString(extra), QStringLiteral("grade.primary")));
  EXPECT_EQ(nodes->last_error(), QStringLiteral("mini-Git journal append failed"));
  auto* error = Find(QStringLiteral("editorNodesCommandError"));
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->property("text").toString(), QStringLiteral("mini-Git journal append failed"));
  EXPECT_FALSE(nodes->incomplete_draft());
  EXPECT_EQ(nodes->backbone_node_ids().size(), 4);
}

TEST_F(EditorNodesPanelQmlTest, HeaderHasNoApplyOrCancelAction) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  EXPECT_NE(Find(QStringLiteral("editorNodesAddButton")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesApplyButton")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesCancelButton")), nullptr);
}

TEST_F(EditorNodesPanelQmlTest, OpenPageAppliesCommittedProjectionOnce) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->has_projection(), 2000);
  ProcessEvents();
  EXPECT_EQ(nodes->completed_projection_apply_count(), 1);
  EXPECT_EQ(adapter->topology_replace_count(), 1);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
}

TEST_F(EditorNodesPanelQmlTest, OrdinaryDraftEditsDoNotReplaceQanTopology) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ProcessEvents();
  const auto applies  = nodes->completed_projection_apply_count();
  const auto replaces = adapter->topology_replace_count();
  ASSERT_TRUE(nodes->addCleanColorGrade());
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(nodes->selected_node_id()) != nullptr, 2000);
  EXPECT_EQ(nodes->completed_projection_apply_count(), applies);
  EXPECT_EQ(adapter->topology_replace_count(), replaces);
  EXPECT_EQ(nodes->snapshot().nodes.size(), 3u);
  EXPECT_EQ(nodes->ActiveNodes().size(), 4u);
}

TEST_F(EditorNodesPanelQmlTest, CloseWithQueuedApplyThenReopenRestoresOnce) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  const auto queued_before = nodes->queued_projection_apply_count();
  ASSERT_NE(backend_.pipeline_document(), nullptr);
  const auto result = backend_.RenameColorGrade(NodeId{"grade.primary"}, "Queued update");
  ASSERT_FALSE(alcedo::EditorSessionResultIsFailure(result.kind));
  EXPECT_GT(nodes->queued_projection_apply_count(), queued_before);

  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) == nullptr, 2000);
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  nodes         = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->has_projection(), 2000);
  ProcessEvents();
  EXPECT_EQ(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, true);
  EXPECT_EQ(nodes->graph_adapter_object(), adapter);
}

TEST_F(EditorNodesPanelQmlTest, NoImageShowsLocalizedEmptyCopyAndHidesTheGraph) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesGraphView")) != nullptr, 2000);
  QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 3, 2000);

  backend_.SetSessionState(alcedo::EditorSessionState::NoImage, false);
  ProcessEvents();

  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(!nodes->has_snapshot(), 2000);
  auto* empty   = Find(QStringLiteral("editorNodesEmptyState"));
  auto* loading = Find(QStringLiteral("editorNodesLoadingState"));
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  auto* add     = Find(QStringLiteral("editorNodesAddButton"));
  ASSERT_NE(empty, nullptr);
  ASSERT_NE(loading, nullptr);
  ASSERT_NE(view, nullptr);
  ASSERT_NE(add, nullptr);
  EXPECT_TRUE(empty->isVisible());
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("Select an image to edit nodes"));
  EXPECT_FALSE(loading->isVisible());
  EXPECT_FALSE(view->isVisible());
  EXPECT_FALSE(add->isEnabled());
  EXPECT_EQ(Find(QStringLiteral("editorNodesApplyButton")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesCancelButton")), nullptr);
}

TEST_F(EditorNodesPanelQmlTest, LoadingHidesThePreviousGraphAndShowsLocalizedCopy) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 3, 2000);
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  ASSERT_TRUE(nodes->has_snapshot());

  backend_.SetSessionState(alcedo::EditorSessionState::Loading, true);
  ProcessEvents();

  QTRY_VERIFY_WITH_TIMEOUT(!nodes->has_snapshot(), 2000);
  auto* empty   = Find(QStringLiteral("editorNodesEmptyState"));
  auto* loading = Find(QStringLiteral("editorNodesLoadingState"));
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(empty, nullptr);
  ASSERT_NE(loading, nullptr);
  ASSERT_NE(view, nullptr);
  EXPECT_FALSE(empty->isVisible());
  EXPECT_TRUE(loading->isVisible());
  EXPECT_EQ(loading->property("text").toString(), QStringLiteral("Loading node graph"));
  EXPECT_FALSE(view->isVisible());
  EXPECT_EQ(AttachedName(loading), QStringLiteral("Loading node graph"));

  backend_.SetSessionState(alcedo::EditorSessionState::Interactive, true);
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(view->isVisible(), 2000);
  EXPECT_FALSE(loading->isVisible());
}

TEST_F(EditorNodesPanelQmlTest, PendingCommandShowsUpdatingCopyThenHidesItWhenIdle) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* pending = Find(QStringLiteral("editorNodesPendingCommand"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->property("text").toString(), QStringLiteral("Updating node graph"));
  EXPECT_FALSE(pending->isVisible());
  EXPECT_FALSE(nodes->command_active());

  bool pending_visible_while_active = false;
  QObject::connect(nodes, &EditorNodeController::CommandStateChanged, [&] {
    if (nodes->command_active()) {
      ProcessEvents();
      pending_visible_while_active = pending->isVisible();
    }
  });
  ASSERT_TRUE(nodes->addCleanColorGrade());
  EXPECT_TRUE(pending_visible_while_active);
  EXPECT_FALSE(nodes->command_active());
  EXPECT_FALSE(pending->isVisible());
}

TEST_F(EditorNodesPanelQmlTest, KeyboardSelectsFitConnectsAndCancelsWithoutApplyCancel) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(view, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(view->isVisible() && view->isEnabled(), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  view->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(view->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Home);
  ProcessEvents();
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"develop"});
  QTest::keyClick(window_, Qt::Key_Down);
  ProcessEvents();
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  QTest::keyClick(window_, Qt::Key_End);
  ProcessEvents();
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"drt"});
  QTest::keyClick(window_, Qt::Key_Up);
  ProcessEvents();
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});

  ASSERT_TRUE(view->setProperty("zoom", 0.25));
  ProcessEvents();
  EXPECT_NEAR(view->property("zoom").toDouble(), 0.25, 0.01);
  QTest::keyClick(window_, Qt::Key_0, Qt::ControlModifier);
  ProcessEvents();
  EXPECT_NE(view->property("zoom").toDouble(), 0.25);

  nodes->selectDevelop();
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_C);
  ProcessEvents();
  EXPECT_TRUE(adapter->keyboard_connect_active());
  EXPECT_EQ(adapter->keyboard_connect_source_id_string(), QStringLiteral("develop"));
  auto* guidance = Find(QStringLiteral("editorNodesEditingGuidance"));
  ASSERT_NE(guidance, nullptr);
  EXPECT_TRUE(guidance->isVisible());
  EXPECT_EQ(guidance->property("text").toString(),
            QStringLiteral("Select a destination node and press Enter"));

  QTest::keyClick(window_, Qt::Key_Escape);
  ProcessEvents();
  EXPECT_FALSE(adapter->keyboard_connect_active());
  EXPECT_FALSE(guidance->isVisible());

  ASSERT_TRUE(nodes->addCleanColorGrade());
  const auto extra = nodes->selected_node_id_string();
  nodes->selectDevelop();
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_C);
  ProcessEvents();
  ASSERT_TRUE(adapter->keyboard_connect_active());
  nodes->selectNode(extra);
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_FALSE(adapter->keyboard_connect_active());
  EXPECT_TRUE(nodes->incomplete_draft());

  nodes->selectDrt();
  view->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_C);
  ProcessEvents();
  EXPECT_FALSE(adapter->keyboard_connect_active());

  EXPECT_EQ(Find(QStringLiteral("editorNodesApplyButton")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesCancelButton")), nullptr);
}

TEST_F(EditorNodesPanelQmlTest, EnterAndSpaceToggleTheFocusedMaskDrawerHeader) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* adapter = Adapter();
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(view, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade, nullptr);
  auto* header = grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawerHeader"));
  auto* drawer = grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawer"));
  ASSERT_NE(header, nullptr);
  ASSERT_NE(drawer, nullptr);
  EXPECT_TRUE(drawer->property("expanded").toBool());
  EXPECT_EQ(AttachedName(header), QStringLiteral("Collapse Masks"));

  header->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(header->hasActiveFocus());
  QTest::keyClick(window_, Qt::Key_Space);
  ProcessEvents();
  EXPECT_FALSE(drawer->property("expanded").toBool());
  EXPECT_EQ(AttachedName(header), QStringLiteral("Expand Masks"));

  header->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(header->hasActiveFocus());
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_TRUE(drawer->property("expanded").toBool());
  EXPECT_EQ(AttachedName(header), QStringLiteral("Collapse Masks"));
}

TEST_F(EditorNodesPanelQmlTest, TabOrderWalksAddGraphAndMaskDrawerHeader) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* add    = Find(QStringLiteral("editorNodesAddButton"));
  auto* view   = Find(QStringLiteral("editorNodesGraphView"));
  auto* header = adapter->NodeFor(NodeId{"grade.primary"})
                     ->getItem()
                     ->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawerHeader"));
  ASSERT_NE(add, nullptr);
  ASSERT_NE(view, nullptr);
  ASSERT_NE(header, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(view->isVisible() && view->activeFocusOnTab(), 2000);
  EXPECT_TRUE(add->activeFocusOnTab());
  EXPECT_TRUE(header->activeFocusOnTab());
  EXPECT_EQ(QQmlProperty(add, QStringLiteral("KeyNavigation.tab"), qmlContext(add))
                .read()
                .value<QObject*>(),
            view);
  EXPECT_EQ(QQmlProperty(view, QStringLiteral("KeyNavigation.backtab"), qmlContext(view))
                .read()
                .value<QObject*>(),
            add);

  add->forceActiveFocus();
  QTest::keyClick(window_, Qt::Key_Tab);
  ProcessEvents();
  EXPECT_TRUE(view->hasActiveFocus());
}

TEST_F(EditorNodesPanelQmlTest, AccessibleNamesCoverActionsAndOmitBannedNodeCopy) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* adapter = Adapter();
  auto* add     = Find(QStringLiteral("editorNodesAddButton"));
  auto* view    = Find(QStringLiteral("editorNodesGraphView"));
  auto* title   = Find(QStringLiteral("editorNodesPanelTitle"));
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(add, nullptr);
  ASSERT_NE(view, nullptr);
  ASSERT_NE(title, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);

  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("Add Color Grade"));
  EXPECT_EQ(title->property("text").toString(), QStringLiteral("Nodes"));
  const auto add_name = AttachedName(add);
  if (!add_name.isEmpty()) {
    EXPECT_EQ(add_name, QStringLiteral("Add Color Grade"));
  }
  const auto view_name = AttachedName(view);
  if (!view_name.isEmpty()) {
    EXPECT_EQ(view_name, QStringLiteral("Nodes graph"));
  }

  QStringList phrases;
  CollectAccessiblePhrases(Find(QStringLiteral("editorNodesPageBody")), &phrases);
  for (const auto& phrase : phrases) {
    EXPECT_FALSE(phrase.contains(QStringLiteral(" · ")));
    EXPECT_FALSE(phrase.contains(QStringLiteral(" | ")));
    EXPECT_NE(phrase, QStringLiteral("On"));
    EXPECT_NE(phrase, QStringLiteral("Off"));
    EXPECT_NE(phrase, QStringLiteral("Active"));
    EXPECT_NE(phrase, QStringLiteral("Inactive"));
    EXPECT_FALSE(phrase.contains(QStringLiteral("1 masks"), Qt::CaseInsensitive));
    EXPECT_FALSE(phrase.contains(QStringLiteral("Exposure")));
    EXPECT_FALSE(phrase.contains(QStringLiteral("#1")));
  }

  if (QAccessible::isActive()) {
    if (auto* iface = QAccessible::queryAccessibleInterface(add)) {
      EXPECT_EQ(iface->text(QAccessible::Name), QStringLiteral("Add Color Grade"));
    }
  }
}

TEST_F(EditorNodesPanelQmlTest, FortyPercentTextExpansionKeepsTitleWrappingInsideThePanel) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  NodesPanelTextExpander expander;
  ASSERT_TRUE(QCoreApplication::installTranslator(&expander));
  engine_.retranslate();
  OpenNodesPage();
  engine_.retranslate();
  ProcessEvents();
  auto* panel = Find(QStringLiteral("editorNodesPageBody"));
  auto* title = Find(QStringLiteral("editorNodesPanelTitle"));
  auto* add   = Find(QStringLiteral("editorNodesAddButton"));
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(add, nullptr);
  EXPECT_TRUE(title->property("text").toString().endsWith(QLatin1Char('W')));
  EXPECT_EQ(title->property("wrapMode").toInt(), 1);
  EXPECT_LE(title->width() + add->width(), panel->width() + 1.0);
  EXPECT_TRUE(add->isVisible());
  QCoreApplication::removeTranslator(&expander);
  engine_.retranslate();
}

}  // namespace
}  // namespace alcedo::ui::test
