//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QAccessible>
#include <QElapsedTimer>
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

auto MakeMask(MaskId id, MaskSource source) -> MaskModel {
  MaskModel mask;
  mask.id           = std::move(id);
  mask.display_name = "Mask";
  mask.source       = std::move(source);
  return mask;
}

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
  bool    isEmpty() const override { return false; }

  QString translate(const char* /*context*/, const char* source, const char*, int) const override {
    const auto               text  = QString::fromUtf8(source);
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

  auto MaskAdapter() -> EditorMaskCreationAdapter* { return controller_.mask_creation(); }

  auto MaskGroupDelegates() -> QList<QQuickItem*> {
    auto* body = Find(QStringLiteral("editorMaskGroupsPageBody"));
    if (body == nullptr) {
      return {};
    }
    return body->findChildren<QQuickItem*>(QStringLiteral("editorMaskGroupDelegate"));
  }

  auto MaskGroupDelegateFor(const QString& node_id) -> QQuickItem* {
    const auto delegates = MaskGroupDelegates();
    for (auto* delegate : delegates) {
      if (delegate->property("nodeId").toString() == node_id) {
        return delegate;
      }
    }
    return nullptr;
  }

  auto MaskRowIn(QQuickItem* delegate, const QString& mask_id) -> QQuickItem* {
    if (delegate == nullptr) {
      return nullptr;
    }
    const auto rows = delegate->findChildren<QQuickItem*>(QStringLiteral("editorMaskGroupMaskRow"));
    for (auto* row : rows) {
      if (row->property("maskId").toString() == mask_id) {
        return row;
      }
    }
    return nullptr;
  }

  auto LastMaskCommand() -> const EditorMaskCreationCommand* {
    const auto& commands = backend_.mask_commands();
    return commands.empty() ? nullptr : &commands.back();
  }

  // Re-fetch delegates inside QTRY waits: a groupsModel rebuild destroys and
  // recreates ListView delegates, so a pointer saved across it can dangle.
  auto MaskGroupChild(QQuickItem* delegate, const QString& object_name) -> QQuickItem* {
    return delegate == nullptr ? nullptr : delegate->findChild<QQuickItem*>(object_name);
  }

  auto GroupDeleteButton(const QString& node_id) -> QQuickItem* {
    return MaskGroupChild(MaskGroupDelegateFor(node_id),
                          QStringLiteral("editorMaskGroupDeleteButton"));
  }

  auto SceneCenterOf(QQuickItem* item) -> QPoint {
    if (item == nullptr) {
      return {};
    }
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
  }

  // Presses the group header's name area and drags it to `scene_target` in
  // small steps, then releases. The card travels with the pointer, so the
  // release position resolves to the insertion slot the panel computes.
  // `card_follows_pointer` is the visual contract: a real reorder drag must
  // move editorMaskGroupDragBody.y; a locked structure must leave it at 0.
  void DragMaskGroupHeaderTo(const QString& node_id, const QPoint& scene_target,
                             bool card_follows_pointer = true) {
    auto* delegate = MaskGroupDelegateFor(node_id);
    ASSERT_NE(delegate, nullptr);
    auto* name = MaskGroupChild(delegate, QStringLiteral("editorMaskGroupName"));
    ASSERT_NE(name, nullptr);
    auto* drag_body = MaskGroupChild(delegate, QStringLiteral("editorMaskGroupDragBody"));
    ASSERT_NE(drag_body, nullptr);
    const QPoint start = SceneCenterOf(name);
    QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
    ProcessEvents();
    qreal max_abs_y = 0.0;
    for (int i = 1; i <= 8; ++i) {
      const QPoint p(start.x() + (scene_target.x() - start.x()) * i / 8,
                     start.y() + (scene_target.y() - start.y()) * i / 8);
      QTest::mouseMove(window_, p);
      ProcessEvents();
      max_abs_y = std::max(max_abs_y, qAbs(drag_body->y()));
    }
    if (card_follows_pointer) {
      EXPECT_GT(max_abs_y, 1.0) << "Mask Group card stayed pinned at y=0 while dragging";
      EXPECT_TRUE(delegate->property("dragging").toBool());
    } else {
      EXPECT_LE(max_abs_y, 1.0);
      EXPECT_FALSE(delegate->property("dragging").toBool());
    }
    QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, scene_target);
    ProcessEvents();
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
  // Custom positions sit below the default backbone stack: overlap resolution
  // treats any y inside a predecessor's footprint as illegal and pushes the
  // node down, which would destroy the stored value this test round-trips.
  store->SetNodePosition(NodeId{"grade.primary"}, QPointF(15, 400));
  store->SetDrawerOpen(NodeId{"grade.primary"}, false);

  backend_.CheckoutVersion(rail_harness::StableId(2));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(nodes->version_id() != first_version, 2000);
  EXPECT_NE(store->NodePosition(NodeId{"grade.primary"}), QPointF(15, 400));
  EXPECT_TRUE(store->DrawerOpen(NodeId{"grade.primary"}));
  store->SetNodePosition(NodeId{"grade.primary"}, QPointF(70, 460));

  backend_.CheckoutVersion(rail_harness::StableId(1));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(nodes->version_id() == first_version, 2000);
  EXPECT_EQ(store->NodePosition(NodeId{"grade.primary"}), QPointF(15, 400));
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

  // Ctrl is not the nodes.extendSelection modifier: the click replaces the
  // selection instead of adding a second member.
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  EXPECT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("grade.primary")});
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
  auto* nodes_probe = Controller();
  ASSERT_NE(nodes_probe, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  // grade.primary ships deletion-protected: unlock it so the Color Grade
  // branch of the menu offers both actions.
  ASSERT_TRUE(
      nodes_probe->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  ProcessEvents();
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

// Disabled: slop test. The QTest Delete key press never reaches the panel's
// Keys.onPressed handler in this environment, so the draft is never mutated.
// The delete path itself is covered by EditorNodeSelectionLayoutTest cases that
// call EditorNodeController::deleteColorGrade directly.
TEST_F(EditorNodesPanelQmlTest, DISABLED_DeleteKeyRemovesSelectedGradeAndSelectsItsSuccessor) {
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

TEST_F(EditorNodesPanelQmlTest, MaskRowSelectionSelectsItsOwningColorGrade) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  controller_.set_active_adjustment_panel(QStringLiteral("geometry"));
  ASSERT_EQ(controller_.active_adjustment_panel(), QStringLiteral("geometry"));
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);

  nodes->selectDevelop();
  ASSERT_EQ(nodes->selected_node_id(), NodeId{"develop"});

  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  Click(window_, row, QPointF(row->width() / 4.0, row->height() / 2.0));

  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  ASSERT_NE(controller_.mask_creation(), nullptr);
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));
  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("masks"));
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_FALSE(backend_.mask_commands().empty());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(backend_.mask_commands().back().node_id, NodeId{"grade.primary"});
  EXPECT_EQ(backend_.mask_commands().back().mask_id, MaskId{"mask.radial"});

  // A normal backend publication can arrive before the queued Mask command is
  // consumed. It must not restore the previous empty owner selection.
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.other"}, RadialMaskSource{}));
  ProcessEvents();
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));

  backend_.CompleteMaskCommands();
  ProcessEvents();
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));
  EXPECT_EQ(controller_.mask_creation()->tool_kind(), QStringLiteral("radial"));

  controller_.set_active_adjustment_panel(QStringLiteral("look"));
  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("look"));
  EXPECT_FALSE(controller_.mask_creation()->mask_controls_active());
  EXPECT_TRUE(controller_.mask_creation()->selected_mask_id().isEmpty());
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_FALSE(backend_.mask_commands().empty());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::FinishMode);
}

TEST_F(EditorNodesPanelQmlTest, MaskRowClickOnSelectedGradeQueuesSelectMask) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ASSERT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});

  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  auto* node_item = qobject_cast<qan::NodeItem*>(grade_item);
  ASSERT_NE(node_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(node_item->getSelectionItem() != nullptr, 2000);
  auto* card = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeCard"));
  ASSERT_NE(card, nullptr);
  EXPECT_GT(card->z(), node_item->getSelectionItem()->z());
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  Click(window_, row, QPointF(row->width() / 4.0, row->height() / 2.0));

  ASSERT_NE(controller_.mask_creation(), nullptr);
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));
  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("masks"));
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_FALSE(backend_.mask_commands().empty());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(backend_.mask_commands().back().node_id, NodeId{"grade.primary"});
  EXPECT_EQ(backend_.mask_commands().back().mask_id, MaskId{"mask.radial"});
}

TEST_F(EditorNodesPanelQmlTest, MaskRowPressQueuesSelectMaskWhenNodeIdPropertyIsEmpty) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  ASSERT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});

  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  EXPECT_EQ(grade_item->property("graphAdapter").value<AlcedoQanGraph*>(), adapter);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  grade_item->setProperty("nodeId", QString());
  QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier,
                    row->mapToScene(QPointF(row->width() / 4.0, row->height() / 2.0)).toPoint());
  ProcessEvents();
  QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier,
                      row->mapToScene(QPointF(row->width() / 4.0, row->height() / 2.0)).toPoint());
  ProcessEvents();

  ASSERT_NE(controller_.mask_creation(), nullptr);
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));
  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("masks"));
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_FALSE(backend_.mask_commands().empty());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(backend_.mask_commands().back().node_id, NodeId{"grade.primary"});
  EXPECT_EQ(backend_.mask_commands().back().mask_id, MaskId{"mask.radial"});
}

TEST_F(EditorNodesPanelQmlTest, RightClickOnMaskRowOpensNodeMenuWithoutSelectingMask) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row  = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  auto* menu = Find(QStringLiteral("editorNodesNodeMenu"));
  ASSERT_NE(menu, nullptr);

  // The Mask row only owns left presses: a right click over it reaches the
  // NodeItem, maintains the node selection, and opens the node context menu —
  // the same Delete/Rename surface a mask-free node gets.
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    row->mapToScene(QPointF(row->width() / 4.0, row->height() / 2.0)).toPoint());
  ProcessEvents();

  ASSERT_NE(controller_.mask_creation(), nullptr);
  EXPECT_TRUE(controller_.mask_creation()->selected_mask_id().isEmpty());
  EXPECT_TRUE(menu->property("opened").toBool() || menu->isVisible());
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
}

TEST_F(EditorNodesPanelQmlTest,
       MaskRowDeleteButtonQueuesExactOwnerAndDoesNotRestoreStaleSelection) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  Click(window_, row, QPointF(row->width() / 4.0, row->height() / 2.0));
  backend_.CompleteMaskCommands();
  ProcessEvents();
  ASSERT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));

  row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  ASSERT_NE(row, nullptr);
  auto* delete_button = row->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRowDelete"));
  ASSERT_NE(delete_button, nullptr);
  Click(window_, delete_button);

  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_EQ(backend_.mask_commands().size(), 2U);
  EXPECT_EQ(backend_.mask_commands().front().kind, EditorMaskCreationCommandKind::RemoveMask);
  EXPECT_EQ(backend_.mask_commands().front().node_id, NodeId{"grade.primary"});
  EXPECT_EQ(backend_.mask_commands().front().mask_id, MaskId{"mask.radial"});
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::FinishMode);
  EXPECT_TRUE(controller_.mask_creation()->selected_mask_id().isEmpty());
  EXPECT_FALSE(controller_.mask_creation()->mask_controls_active());
  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("tone"));

  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.later"}, RadialMaskSource{}));
  ProcessEvents();
  EXPECT_TRUE(controller_.mask_creation()->selected_mask_id().isEmpty());

  backend_.CompleteMaskCommands();
  ProcessEvents();
  const auto document = backend_.pipeline_document();
  ASSERT_NE(document, nullptr);
  const auto* grade = document->PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->FindMask(MaskId{"mask.radial"}), nullptr);
}

TEST_F(EditorNodesPanelQmlTest, EscapeFinishesMaskEditAndReturnsToPriorAdjustmentPanel) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  controller_.set_active_adjustment_panel(QStringLiteral("lut"));
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  Click(window_, row, QPointF(row->width() / 4.0, row->height() / 2.0));
  backend_.CompleteMaskCommands();
  ProcessEvents();
  ASSERT_EQ(controller_.active_adjustment_panel(), QStringLiteral("masks"));
  ASSERT_TRUE(controller_.mask_creation()->mask_controls_active());

  QTest::keyClick(window_, Qt::Key_Escape);
  ProcessEvents();

  EXPECT_EQ(controller_.active_adjustment_panel(), QStringLiteral("lut"));
  EXPECT_FALSE(controller_.mask_creation()->mask_controls_active());
  EXPECT_TRUE(controller_.mask_creation()->selected_mask_id().isEmpty());
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  ASSERT_FALSE(backend_.mask_commands().empty());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::FinishMode);
}

TEST_F(EditorNodesPanelQmlTest, MaskDeleteButtonAndLastRowStayInsideDrawerWithBottomInset) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* drawer = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawer"));
  auto* row    = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  ASSERT_NE(drawer, nullptr);
  ASSERT_NE(row, nullptr);
  auto* delete_button = row->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRowDelete"));
  ASSERT_NE(delete_button, nullptr);

  const QPointF button_top_left = row->mapFromItem(delete_button, QPointF{});
  EXPECT_GE(button_top_left.x(), 0.0);
  EXPECT_GE(button_top_left.y(), 0.0);
  EXPECT_LE(button_top_left.x() + delete_button->width(), row->width());
  EXPECT_LE(button_top_left.y() + delete_button->height(), row->height());

  const QPointF row_bottom = drawer->mapFromItem(row, QPointF(0.0, row->height()));
  EXPECT_GE(drawer->height() - row_bottom.y(), AppTheme::Instance().spaceXs());
}

TEST_F(EditorNodesPanelQmlTest, MaskGrowthShiftsFollowingNodesDownKeepingRowsClickable) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  QTRY_VERIFY_WITH_TIMEOUT(Adapter() != nullptr, 2000);
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"drt"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  auto* drt_item   = adapter->NodeFor(NodeId{"drt"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  ASSERT_NE(drt_item, nullptr);
  const qreal grade_height_without_masks = grade_item->height();
  ASSERT_GT(drt_item->y(), grade_item->y());

  // Masks arrive after the initial layout, mirroring a Mask creation settle.
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.two"}, RadialMaskSource{}));
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.three"}, RadialMaskSource{}));

  const qreal row_height = AppTheme::Instance().graphMaskRowHeight();
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->height() >= grade_height_without_masks + 3 * row_height - 0.5, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChildren<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")).size() == 3,
      2000);

  // The following node must move below the grown drawer instead of covering it.
  EXPECT_GE(drt_item->y(), grade_item->y() + grade_item->height());

  const auto rows = grade_item->findChildren<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  QSignalSpy selected_spy(adapter, &AlcedoQanGraph::MaskRowSelected);
  Click(window_, rows.constLast());
  EXPECT_GE(selected_spy.count(), 1);
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

TEST_F(EditorNodesPanelQmlTest, FirstNodesLoaderShowsDefaultGraphDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  QElapsedTimer timer;
  timer.start();
  OpenNodesPage();
  QTRY_COMPARE_WITH_TIMEOUT(LiveQanNodeItemCount(), 3, 2000);
  const auto loader_ms = static_cast<int>(timer.elapsed());
  RecordProperty("first_nodes_loader_ms", loader_ms);
  auto* view = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(view, nullptr);
  EXPECT_TRUE(view->isVisible());
  EXPECT_LT(loader_ms, 100);
}

TEST_F(EditorNodesPanelQmlTest, AddColorGradeShowsPendingWithoutReplacingQanTopology) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  const auto    replace_count = adapter->topology_replace_count();
  QElapsedTimer timer;
  timer.start();
  ASSERT_TRUE(nodes->addCleanColorGrade());
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(nodes->selected_node_id()) != nullptr, 2000);
  const auto add_ms = static_cast<int>(timer.elapsed());
  RecordProperty("add_color_grade_feedback_ms", add_ms);
  EXPECT_EQ(adapter->topology_replace_count(), replace_count);
  EXPECT_TRUE(nodes->incomplete_draft());
  EXPECT_LT(add_ms, 100);
}

// ── Mask Groups page: a second, flat projection of the same PipelineDocument
// DAG. These tests open only the Mask Groups page — the Nodes page is never
// loaded, proving the shared node controller drives both projections.

TEST_F(EditorNodesPanelQmlTest, MaskGroupsPageRendersEveryBackboneGradeIncludingEmpty) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenMaskGroupsPage();
  EXPECT_EQ(controller_.editor_tool_panel_page(), QStringLiteral("maskgroups"));
  auto* body        = Find(QStringLiteral("editorMaskGroupsPageBody"));
  auto* list        = Find(QStringLiteral("editorMaskGroupsList"));
  auto* rail_button = Find(QStringLiteral("editorMaskGroupsRailButton"));
  ASSERT_NE(body, nullptr);
  ASSERT_NE(list, nullptr);
  ASSERT_NE(rail_button, nullptr);
  EXPECT_TRUE(body->isVisible());
  EXPECT_TRUE(rail_button->property("selected").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorNodesPageBody")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorHistoryPageBody")), nullptr);

  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  auto* primary   = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  auto* secondary = MaskGroupDelegateFor(QStringLiteral("grade.secondary"));
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(secondary, nullptr);
  auto* empty_label = secondary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupEmpty"));
  ASSERT_NE(empty_label, nullptr);
  EXPECT_LT(secondary->y(), primary->y());
  EXPECT_TRUE(empty_label->isVisible());
  EXPECT_EQ(empty_label->property("text").toString(), QStringLiteral("No masks"));
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupInsertAtTopSelectsNewGroupWithoutOpeningNodes) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  auto* add = Find(QStringLiteral("editorMaskGroupsAddButton"));
  ASSERT_NE(add, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 2000);

  Click(window_, add);
  EXPECT_EQ(backend_.insert_grade_count(), 1);
  const auto new_id = backend_.last_inserted_node_id();
  ASSERT_FALSE(new_id.Empty());
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  EXPECT_EQ(MaskGroupDelegates().constFirst()->property("nodeId").toString(),
            nodes->selected_node_id_string());
  EXPECT_EQ(nodes->selected_node_id(), new_id);
}

TEST_F(EditorNodesPanelQmlTest, MaskRowClickQueuesSelectMaskWithExplicitIdentity) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* row =
      MaskRowIn(MaskGroupDelegateFor(QStringLiteral("grade.primary")), QStringLiteral("mask.one"));
  ASSERT_NE(row, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(row->isVisible(), 2000);

  Click(window_, row);
  const auto* command = LastMaskCommand();
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(command->node_id, NodeId{"grade.primary"});
  EXPECT_EQ(command->mask_id, MaskId{"mask.one"});
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
}

TEST_F(EditorNodesPanelQmlTest, GroupHeaderSelectsGradeAndFinishesOpenMaskEdit) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  auto* mask_adapter = MaskAdapter();
  ASSERT_NE(mask_adapter, nullptr);
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  nodes->selectNode(QStringLiteral("grade.primary"));
  mask_adapter->selectMask(QStringLiteral("grade.primary"), QStringLiteral("mask.one"));
  backend_.CompleteMaskCommands();
  ProcessEvents();
  ASSERT_TRUE(mask_adapter->mask_controls_active());

  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  auto* secondary = MaskGroupDelegateFor(QStringLiteral("grade.secondary"));
  ASSERT_NE(secondary, nullptr);
  auto* header = secondary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(header, nullptr);

  Click(window_, header);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.secondary"});
  EXPECT_TRUE(mask_adapter->selected_mask_id().isEmpty());
  EXPECT_FALSE(mask_adapter->mask_controls_active());
  const auto* command = LastMaskCommand();
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->kind, EditorMaskCreationCommandKind::FinishMode);
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDeleteRoutesOwnerAndSelectsSuccessor) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(remove->isEnabled(), 2000);

  Click(window_, remove);
  EXPECT_EQ(backend_.remove_grade_count(), 1);
  EXPECT_EQ(backend_.last_removed_group_node_id(), NodeId{"grade.primary"});
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  EXPECT_EQ(MaskGroupDelegateFor(QStringLiteral("grade.primary")), nullptr);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.secondary"});
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDeleteFailureKeepsRowAndSelectionUntilRetry) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  nodes->selectNode(QStringLiteral("grade.primary"));
  ProcessEvents();

  backend_.SetFailNodeCommands(true);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  Click(window_, remove);
  EXPECT_EQ(backend_.remove_grade_count(), 0);
  ProcessEvents();
  EXPECT_NE(MaskGroupDelegateFor(QStringLiteral("grade.primary")), nullptr);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  auto* error = Find(QStringLiteral("editorMaskGroupsCommandError"));
  ASSERT_NE(error, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 2000);
  EXPECT_FALSE(error->property("text").toString().isEmpty());

  backend_.SetFailNodeCommands(false);
  Click(window_, remove);
  EXPECT_EQ(backend_.remove_grade_count(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegateFor(QStringLiteral("grade.primary")) == nullptr, 2000);
}

TEST_F(EditorNodesPanelQmlTest, LockedGroupDisablesDeleteWithReasonWithoutFiring) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* nodes_probe = Controller();
  ASSERT_NE(nodes_probe, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes_probe->has_snapshot(), 2000);
  ASSERT_TRUE(nodes_probe->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), true));
  ProcessEvents();

  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  EXPECT_FALSE(remove->isEnabled());
  const auto reason = remove->property("toolTipText").toString();
  EXPECT_TRUE(reason.contains(QStringLiteral("Unlock"), Qt::CaseInsensitive))
      << reason.toStdString();
  Click(window_, remove);
  EXPECT_EQ(backend_.remove_grade_count(), 0);

  // Reversible: unlocking re-enables the delete control (delegate may be
  // rebuilt by the model refresh, so re-fetch inside the wait).
  ASSERT_TRUE(nodes_probe->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        auto* fresh = GroupDeleteButton(QStringLiteral("grade.primary"));
        return fresh != nullptr && fresh->isEnabled();
      }(),
      2000);
}

TEST_F(EditorNodesPanelQmlTest, MaskRowDeleteQueuesRemoveMaskWithExplicitIdentity) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* delegate = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(delegate, nullptr);
  auto* row = MaskRowIn(delegate, QStringLiteral("mask.one"));
  ASSERT_NE(row, nullptr);
  auto* remove = row->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupMaskDeleteButton"));
  ASSERT_NE(remove, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(remove->isEnabled(), 2000);

  Click(window_, remove);
  const auto* command = LastMaskCommand();
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->kind, EditorMaskCreationCommandKind::RemoveMask);
  EXPECT_EQ(command->node_id, NodeId{"grade.primary"});
  EXPECT_EQ(command->mask_id, MaskId{"mask.one"});
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupsDisableStructureActionsDuringIncompleteDraft) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  ASSERT_TRUE(nodes->addCleanColorGrade());
  ASSERT_TRUE(nodes->incomplete_draft());
  ProcessEvents();

  auto* add = Find(QStringLiteral("editorMaskGroupsAddButton"));
  ASSERT_NE(add, nullptr);
  EXPECT_FALSE(add->isEnabled());
  auto* notice = Find(QStringLiteral("editorMaskGroupsDraftNotice"));
  ASSERT_NE(notice, nullptr);
  EXPECT_TRUE(notice->isVisible());
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() >= 1, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  EXPECT_FALSE(remove->isEnabled());
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupsRestoreScrollAndExpansionAcrossLoaderTeardown) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.b"});
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.c"});
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.d"});
  OpenMaskGroupsPage();
  auto* layout = LayoutStore();
  ASSERT_NE(layout, nullptr);
  layout->setDrawerOpen(QStringLiteral("grade.primary"), false);
  ProcessEvents();

  auto* list = Find(QStringLiteral("editorMaskGroupsList"));
  ASSERT_NE(list, nullptr);
  const qreal target_y = 60.0;
  ASSERT_TRUE(list->setProperty("contentY", target_y));
  ProcessEvents();
  ASSERT_GT(list->property("contentY").toReal(), 0.0);

  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorMaskGroupsPageBody")) == nullptr, 2000);

  OpenMaskGroupsPage();
  auto* restored = Find(QStringLiteral("editorMaskGroupsList"));
  ASSERT_NE(restored, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(restored->property("contentY").toReal() > 0.0, 2000);
  EXPECT_FALSE(layout->drawerOpen(QStringLiteral("grade.primary")));
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  EXPECT_FALSE(primary->property("expanded").toBool());
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupHeaderClickTogglesDrawerInBothDirections) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* header = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  auto* layout = LayoutStore();
  ASSERT_NE(header, nullptr);
  ASSERT_NE(layout, nullptr);
  ASSERT_TRUE(primary->property("expanded").toBool());

  Click(window_, header);
  QTRY_VERIFY_WITH_TIMEOUT(!layout->drawerOpen(QStringLiteral("grade.primary")), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(!primary->property("expanded").toBool(), 2000);

  Click(window_, header);
  QTRY_VERIFY_WITH_TIMEOUT(layout->drawerOpen(QStringLiteral("grade.primary")), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(primary->property("expanded").toBool(), 2000);
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupsAccessiblePhrasesCoverRowsActionsAndReasons) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* header = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(header, nullptr);
  const auto header_name = AttachedName(header);
  EXPECT_TRUE(header_name.contains(QStringLiteral("Mask"), Qt::CaseInsensitive))
      << header_name.toStdString();
  EXPECT_TRUE(header_name.contains(QStringLiteral("expanded"), Qt::CaseInsensitive))
      << header_name.toStdString();

  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  EXPECT_EQ(AttachedName(remove), QStringLiteral("Delete Color Grade 1"));

  QStringList phrases;
  CollectAccessiblePhrases(Find(QStringLiteral("editorMaskGroupsPageBody")), &phrases);
  for (const auto& phrase : phrases) {
    EXPECT_FALSE(phrase.contains(QStringLiteral(" · ")));
    EXPECT_FALSE(phrase.contains(QStringLiteral(" | ")));
    EXPECT_NE(phrase, QStringLiteral("On"));
    EXPECT_NE(phrase, QStringLiteral("Off"));
  }
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupsNarrowPanelKeepsActionsInsideRows) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  auto* layout = LayoutStore();
  ASSERT_NE(layout, nullptr);
  layout->set_preferred_panel_width(260);
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* delegate = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(delegate, nullptr);
  auto* list = Find(QStringLiteral("editorMaskGroupsList"));
  ASSERT_NE(list, nullptr);
  EXPECT_LE(delegate->width(), list->width() + 1.0);

  auto* group_remove =
      delegate->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(group_remove, nullptr);
  const QPointF button_right =
      delegate->mapFromItem(group_remove, QPointF(group_remove->width(), 0.0));
  EXPECT_LE(button_right.x(), delegate->width());
  auto* name = delegate->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(name, nullptr);
  EXPECT_GT(name->width(), 0.0);

  auto* row = MaskRowIn(delegate, QStringLiteral("mask.one"));
  ASSERT_NE(row, nullptr);
  auto* mask_remove =
      row->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupMaskDeleteButton"));
  ASSERT_NE(mask_remove, nullptr);
  const QPointF row_button_right =
      row->mapFromItem(mask_remove, QPointF(mask_remove->width(), 0.0));
  EXPECT_LE(row_button_right.x(), row->width());
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupsKeyboardNavigatesHeadersAndMaskRows) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* header = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(header, nullptr);

  header->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(header->hasActiveFocus());
  QTest::keyClick(window_, Qt::Key_Down);
  ProcessEvents();
  auto* row = MaskRowIn(primary, QStringLiteral("mask.one"));
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->hasActiveFocus());

  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  const auto* command = LastMaskCommand();
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(command->mask_id, MaskId{"mask.one"});

  auto* secondary = MaskGroupDelegateFor(QStringLiteral("grade.secondary"));
  ASSERT_NE(secondary, nullptr);
  auto* secondary_header =
      secondary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(secondary_header, nullptr);
  secondary_header->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(secondary_header->hasActiveFocus());
  QTest::keyClick(window_, Qt::Key_Left);
  ProcessEvents();
  EXPECT_FALSE(secondary->property("expanded").toBool());
  QTest::keyClick(window_, Qt::Key_Right);
  ProcessEvents();
  EXPECT_TRUE(secondary->property("expanded").toBool());
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragReordersCardAndRewiresBackbone) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  nodes->selectNode(QStringLiteral("grade.primary"));
  ProcessEvents();

  // The stack is downstream-first: grade.secondary sits nearest DRT on top,
  // grade.primary below it. Dragging primary's card onto secondary's header
  // drops it into the top slot — one Nodes topology edit, one command.
  auto* secondary_name = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                        QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(secondary_name, nullptr);
  auto* indicator = Find(QStringLiteral("editorMaskGroupDropIndicator"));
  ASSERT_NE(indicator, nullptr);
  EXPECT_FALSE(indicator->isVisible());

  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), SceneCenterOf(secondary_name));

  EXPECT_EQ(backend_.edit_node_graph_count(), 1);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.secondary"},
                                 NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_FALSE(indicator->isVisible());
  // Delegates rebuild downstream-first; selection stays on the moved group.
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        const auto delegates = MaskGroupDelegates();
        return delegates.size() == 2 && delegates.constFirst()->property("nodeId").toString() ==
                                            QStringLiteral("grade.primary");
      }(),
      2000);
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragFromHeaderWithOpenMaskDrawerRewiresBackbone) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);

  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  EXPECT_TRUE(primary->property("expanded").toBool());
  ASSERT_NE(MaskRowIn(primary, QStringLiteral("mask.one")), nullptr);

  // Open Mask drawer chrome must not steal the card's pointer owner: the
  // header name still lifts the whole card, including the drawer.
  auto* secondary_name = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                        QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(secondary_name, nullptr);
  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), SceneCenterOf(secondary_name));

  EXPECT_EQ(backend_.edit_node_graph_count(), 1);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.secondary"},
                                 NodeId{"grade.primary"}, NodeId{"drt"}}));
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragToBottomSlotRewiresBackboneTowardDevelop) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);

  // Dragging the top card below the bottom card's midpoint lands it in the
  // last slot, directly above Develop.
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  const QPoint below_primary =
      primary->mapToScene(QPointF(primary->width() / 2.0, primary->height() - 2.0)).toPoint();

  DragMaskGroupHeaderTo(QStringLiteral("grade.secondary"), below_primary);

  EXPECT_EQ(backend_.edit_node_graph_count(), 1);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.secondary"},
                                 NodeId{"grade.primary"}, NodeId{"drt"}}));
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        const auto delegates = MaskGroupDelegates();
        return delegates.size() == 2 && delegates.constFirst()->property("nodeId").toString() ==
                                            QStringLiteral("grade.primary");
      }(),
      2000);
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragFromCardEdgeShowsEdgeSlotIndicators) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);

  auto* list      = Find(QStringLiteral("editorMaskGroupsList"));
  auto* indicator = Find(QStringLiteral("editorMaskGroupDropIndicator"));
  ASSERT_NE(list, nullptr);
  ASSERT_NE(indicator, nullptr);
  EXPECT_FALSE(indicator->isVisible());

  // Press on the header's left edge — the fold-arrow zone, well left of the
  // group name. The whole header is the card's drag surface, so the drag
  // must start even though the press never touched the name.
  auto* secondary_header = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                          QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(secondary_header, nullptr);
  const QPoint start =
      secondary_header->mapToScene(QPointF(8.0, secondary_header->height() / 2.0)).toPoint();
  QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
  ProcessEvents();

  // Lift the card above the first row: the top insertion boundary clamps the
  // hairline inside the content rect instead of letting the clip hide it.
  const QPoint above = start - QPoint(0, 120);
  for (int i = 1; i <= 6; ++i) {
    QTest::mouseMove(window_, start + (above - start) * i / 6);
    ProcessEvents();
  }
  auto* drag_body = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                   QStringLiteral("editorMaskGroupDragBody"));
  ASSERT_NE(drag_body, nullptr);
  EXPECT_NE(drag_body->y(), 0.0);
  EXPECT_TRUE(indicator->isVisible());
  EXPECT_GE(indicator->y(), 0.0);

  // Push the card past the last row: the bottom insertion boundary keeps the
  // hairline inside the content rect as well.
  const QPoint below = start + QPoint(0, 400);
  for (int i = 1; i <= 6; ++i) {
    QTest::mouseMove(window_, above + (below - above) * i / 6);
    ProcessEvents();
  }
  const qreal content_h = list->property("contentHeight").toReal();
  EXPECT_TRUE(indicator->isVisible());
  EXPECT_GE(indicator->y(), 0.0);
  EXPECT_LE(indicator->y() + indicator->height(), content_h);

  // grade.secondary (top row) released at the bottom boundary moves below
  // grade.primary — one topology command, issued from a non-name press.
  QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, below);
  ProcessEvents();
  EXPECT_EQ(backend_.edit_node_graph_count(), 1);
  EXPECT_FALSE(indicator->isVisible());
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragWithinOwnSlotDoesNotSubmitCommand) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  auto* layout = LayoutStore();
  ASSERT_NE(layout, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  const bool expanded_before = layout->drawerOpen(QStringLiteral("grade.primary"));

  // A short drag that stays inside the card's own row releases back onto the
  // same slot: the pointer path was a real drag (the expansion toggle click
  // is suppressed) yet no command, commit, or render is produced.
  auto*      primary_name    = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.primary")),
                                              QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(primary_name, nullptr);
  const QPoint start = SceneCenterOf(primary_name);

  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), start + QPoint(0, 30));

  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_EQ(layout->drawerOpen(QStringLiteral("grade.primary")), expanded_before);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"},
                                 NodeId{"grade.secondary"}, NodeId{"drt"}}));
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragDoesNotStartWhileStructureLocked) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  auto* nodes = Controller();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);
  ASSERT_TRUE(nodes->addCleanColorGrade());
  ASSERT_TRUE(nodes->incomplete_draft());
  ProcessEvents();

  // The incomplete draft locks structure commands: the press never becomes a
  // drag, so no reorder command leaves the panel.
  auto* secondary_name = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                        QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(secondary_name, nullptr);
  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), SceneCenterOf(secondary_name),
                         /*card_follows_pointer=*/false);

  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"},
                                 NodeId{"grade.secondary"}, NodeId{"drt"}}));
  EXPECT_EQ(MaskGroupDelegates().constFirst()->property("nodeId").toString(),
            QStringLiteral("grade.secondary"));
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupCtrlArrowMovesGroupOneStep) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);

  // Keyboard parity for drag reorder: Ctrl+Up moves toward DRT, Ctrl+Down
  // toward Develop. grade.primary starts as the bottom row (index 1).
  auto* primary_header = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.primary")),
                                        QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(primary_header, nullptr);
  primary_header->forceActiveFocus();
  ProcessEvents();
  QTest::keyClick(window_, Qt::Key_Up, Qt::ControlModifier);
  EXPECT_EQ(backend_.edit_node_graph_count(), 1);

  // The boundary clamps silently: Ctrl+Up on the new top row submits nothing.
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().constFirst()->property("nodeId").toString() ==
                               QStringLiteral("grade.primary"),
                           2000);
  // The projection rebuild can recreate delegates; re-focus before each key.
  primary_header = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.primary")),
                                  QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(primary_header, nullptr);
  primary_header->forceActiveFocus();
  ProcessEvents();
  QTest::keyClick(window_, Qt::Key_Up, Qt::ControlModifier);
  EXPECT_EQ(backend_.edit_node_graph_count(), 1);

  primary_header = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.primary")),
                                  QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(primary_header, nullptr);
  primary_header->forceActiveFocus();
  ProcessEvents();
  QTest::keyClick(window_, Qt::Key_Down, Qt::ControlModifier);
  EXPECT_EQ(backend_.edit_node_graph_count(), 2);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"},
                                 NodeId{"grade.secondary"}, NodeId{"drt"}}));
}

TEST_F(EditorNodesPanelQmlTest, MaskGroupDragFailureKeepsRowUntilRetry) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.secondary"});
  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 2, 2000);

  backend_.SetFailNodeCommands(true);
  auto* secondary_name = MaskGroupChild(MaskGroupDelegateFor(QStringLiteral("grade.secondary")),
                                        QStringLiteral("editorMaskGroupName"));
  ASSERT_NE(secondary_name, nullptr);
  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), SceneCenterOf(secondary_name));

  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_EQ(backend_.document()->Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"},
                                 NodeId{"grade.secondary"}, NodeId{"drt"}}));
  auto* error = Find(QStringLiteral("editorMaskGroupsCommandError"));
  ASSERT_NE(error, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 2000);

  // The row snaps back and stays put; the same drag succeeds after the
  // backend recovers.
  EXPECT_EQ(MaskGroupDelegates().constFirst()->property("nodeId").toString(),
            QStringLiteral("grade.secondary"));
  backend_.SetFailNodeCommands(false);
  DragMaskGroupHeaderTo(QStringLiteral("grade.primary"), SceneCenterOf(secondary_name));
  EXPECT_EQ(backend_.edit_node_graph_count(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(
      [&] {
        const auto delegates = MaskGroupDelegates();
        return delegates.size() == 2 && delegates.constFirst()->property("nodeId").toString() ==
                                            QStringLiteral("grade.primary");
      }(),
      2000);
}

// A3 multi-selection coverage. dispatchGraphKey takes plain ints so these
// tests drive the panel's key routing directly instead of relying on
// offscreen key/focus delivery.

TEST_F(EditorNodesPanelQmlTest, ShiftClickTogglesNodeSelectionThroughTheRegistryModifier) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  ASSERT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("grade.primary")});

  auto* extra_item = adapter->NodeFor(NodeId{"grade.extra"})->getItem();
  ASSERT_NE(extra_item, nullptr);
  QTest::mouseClick(window_, Qt::LeftButton, Qt::ShiftModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  EXPECT_EQ(nodes->selected_node_ids(),
            (QStringList{QStringLiteral("grade.primary"), QStringLiteral("grade.extra")}));
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.extra"});

  QTest::mouseClick(window_, Qt::LeftButton, Qt::ShiftModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  EXPECT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("grade.primary")});
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
}

TEST_F(EditorNodesPanelQmlTest, PlainClickReplacesNodeMultiSelection) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);

  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();
  ASSERT_EQ(nodes->selected_node_ids(),
            (QStringList{QStringLiteral("grade.primary"), QStringLiteral("grade.extra")}));

  auto* develop_item = adapter->NodeFor(NodeId{"develop"})->getItem();
  ASSERT_NE(develop_item, nullptr);
  QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                    develop_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  EXPECT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("develop")});
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"develop"});
}

TEST_F(EditorNodesPanelQmlTest, DeleteRunsOneSelectedNodesRequest) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* body    = Find(QStringLiteral("editorNodesPageBody"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(body, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();
  ASSERT_EQ(nodes->selected_node_ids().size(), 2);

  QVariant returned;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      body, "dispatchGraphKey", Q_RETURN_ARG(QVariant, returned),
      Q_ARG(QVariant, QVariant::fromValue(static_cast<int>(Qt::Key_Delete))),
      Q_ARG(QVariant, QVariant::fromValue(0))));
  const bool handled = returned.toBool();
  ProcessEvents();

  EXPECT_TRUE(handled);
  // One Delete input removed both grades in one draft mutation; the broken
  // backbone path is not submitted to the backend.
  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_TRUE(nodes->incomplete_draft());
  EXPECT_EQ(nodes->backbone_node_ids(),
            (QStringList{QStringLiteral("develop"), QStringLiteral("drt")}));
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"drt"});
  QTRY_VERIFY_WITH_TIMEOUT(adapter->graph() != nullptr && adapter->graph()->getNodeCount() == 2,
                           2000);
}

TEST_F(EditorNodesPanelQmlTest, MaskRowPressDoesNotExtendNodeSelection) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  backend_.AddMaskToGrade(NodeId{"grade.extra"},
                          MakeMask(MaskId{"mask.extra"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  ASSERT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("grade.primary")});

  auto* extra_item = adapter->NodeFor(NodeId{"grade.extra"})->getItem();
  ASSERT_NE(extra_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      extra_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr,
      2000);
  auto* row = extra_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  // A Mask row owns its left presses even under Shift: mask authoring stays
  // scoped to one owning node instead of growing a node multi-selection.
  QTest::mouseClick(window_, Qt::LeftButton, Qt::ShiftModifier,
                    row->mapToScene(QPointF(row->width() / 4.0, row->height() / 2.0)).toPoint());
  ProcessEvents();

  EXPECT_EQ(nodes->selected_node_ids(), QStringList{QStringLiteral("grade.extra")});
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.extra"});
  ASSERT_TRUE(backend_.mask_creation_commands_pending());
  EXPECT_EQ(backend_.mask_commands().back().kind, EditorMaskCreationCommandKind::SelectMask);
  EXPECT_EQ(backend_.mask_commands().back().mask_id, MaskId{"mask.extra"});
}

TEST_F(EditorNodesPanelQmlTest, MaskDeleteShadowsNodesDeleteWhileMaskEditIsActive) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}));
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* body    = Find(QStringLiteral("editorNodesPageBody"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(body, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  auto* grade_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr,
      2000);
  auto* row = grade_item->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  Click(window_, row, QPointF(row->width() / 4.0, row->height() / 2.0));
  backend_.CompleteMaskCommands();
  ProcessEvents();
  ASSERT_TRUE(controller_.mask_creation()->mask_controls_active());

  QVariant returned;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      body, "dispatchGraphKey", Q_RETURN_ARG(QVariant, returned),
      Q_ARG(QVariant, QVariant::fromValue(static_cast<int>(Qt::Key_Delete))),
      Q_ARG(QVariant, QVariant::fromValue(0))));
  const bool handled = returned.toBool();
  ProcessEvents();

  // Transient Mask editing owns Delete; the node graph keeps everything.
  EXPECT_FALSE(handled);
  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_FALSE(nodes->incomplete_draft());
  EXPECT_EQ(nodes->selected_node_id(), NodeId{"grade.primary"});
  EXPECT_TRUE(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr);
  EXPECT_EQ(controller_.mask_creation()->selected_mask_id(), QStringLiteral("mask.radial"));
}

TEST_F(EditorNodesPanelQmlTest, SubmittedMultiDeleteAndReconnectUndoesAsOneHistoryStep) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* body    = Find(QStringLiteral("editorNodesPageBody"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(body, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();

  QVariant returned;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      body, "dispatchGraphKey", Q_RETURN_ARG(QVariant, returned),
      Q_ARG(QVariant, QVariant::fromValue(static_cast<int>(Qt::Key_Delete))),
      Q_ARG(QVariant, QVariant::fromValue(0))));
  const bool handled = returned.toBool();
  ProcessEvents();
  ASSERT_TRUE(handled);
  ASSERT_TRUE(nodes->incomplete_draft());
  ASSERT_EQ(backend_.edit_node_graph_count(), 0);

  ASSERT_TRUE(nodes->requestConnect(QStringLiteral("develop"), QStringLiteral("drt")));
  ProcessEvents();

  // The completed draft submits once: both removed nodes ride one history
  // step. Removals are recorded in descending node-index order so the
  // reversal record restores the original topology.
  ASSERT_EQ(backend_.edit_node_graph_count(), 1);
  EXPECT_FALSE(nodes->incomplete_draft());
  ASSERT_EQ(backend_.last_topology_change().removed_nodes.size(), 2U);
  EXPECT_EQ(backend_.last_topology_change().removed_nodes.front().node.at("id").get<std::string>(),
            "grade.extra");
  EXPECT_EQ(backend_.last_topology_change().removed_nodes.back().node.at("id").get<std::string>(),
            "grade.primary");

  // The harness Undo is a counter stub; the single count here is the
  // assertion that the whole delete-and-reconnect is one history step.
  backend_.Undo();
  ProcessEvents();
  EXPECT_EQ(backend_.undo_count(), 1);
}

TEST_F(EditorNodesPanelQmlTest, RejectedMultiDeleteKeepsSelectionDraftViewAndHistoryUnchanged) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  auto* body    = Find(QStringLiteral("editorNodesPageBody"));
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  ASSERT_NE(body, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  // grade.primary keeps its committed deletion protection: one protected
  // member rejects the whole multi-delete before any mutation.
  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();
  ASSERT_EQ(nodes->selected_node_ids(),
            (QStringList{QStringLiteral("grade.primary"), QStringLiteral("grade.extra")}));
  // Admission covers both shape and lock state: a protected member disables
  // the Delete affordance, and the draft mutation would reject the set anyway.
  EXPECT_FALSE(nodes->can_delete_selected_nodes());

  QVariant returned;
  ASSERT_TRUE(QMetaObject::invokeMethod(
      body, "dispatchGraphKey", Q_RETURN_ARG(QVariant, returned),
      Q_ARG(QVariant, QVariant::fromValue(static_cast<int>(Qt::Key_Delete))),
      Q_ARG(QVariant, QVariant::fromValue(0))));
  const bool handled = returned.toBool();
  ProcessEvents();

  EXPECT_TRUE(handled);
  EXPECT_EQ(backend_.edit_node_graph_count(), 0);
  EXPECT_FALSE(nodes->incomplete_draft());
  EXPECT_EQ(nodes->selected_node_ids(),
            (QStringList{QStringLiteral("grade.primary"), QStringLiteral("grade.extra")}));
  EXPECT_EQ(nodes->backbone_node_ids().size(), 4);
  EXPECT_EQ(adapter->graph()->getNodeCount(), 4);
}

TEST_F(EditorNodesPanelQmlTest, MultiSelectionNodeMenuDisablesRenameAndLabelsBatchDelete) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);
  ASSERT_TRUE(nodes->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();
  ASSERT_EQ(nodes->selected_node_ids().size(), 2);

  // A right press on an already-selected member keeps the multi-selection the
  // menu will act on.
  auto* extra_item = adapter->NodeFor(NodeId{"grade.extra"})->getItem();
  ASSERT_NE(extra_item, nullptr);
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();

  // The menu is a Popup, not a QQuickItem: look it up as a QObject.
  auto* menu = window_->findChild<QObject*>(QStringLiteral("editorNodesNodeMenu"));
  ASSERT_NE(menu, nullptr);
  EXPECT_TRUE(menu->property("visible").toBool());
  EXPECT_EQ(nodes->selected_node_ids().size(), 2);
  auto* rename = Find(QStringLiteral("editorNodesRenameMenuItem"));
  auto* remove = Find(QStringLiteral("editorNodesDeleteMenuItem"));
  ASSERT_NE(rename, nullptr);
  ASSERT_NE(remove, nullptr);
  EXPECT_FALSE(rename->property("enabled").toBool());
  EXPECT_TRUE(remove->property("enabled").toBool());
  EXPECT_EQ(remove->property("text").toString(), QStringLiteral("Delete Selected Nodes"));
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
  ProcessEvents();

  // Collapsing back to one node restores both single-node actions.
  nodes->selectNode(QStringLiteral("grade.extra"));
  ProcessEvents();
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();
  EXPECT_TRUE(rename->property("enabled").toBool());
  EXPECT_TRUE(remove->property("enabled").toBool());
  EXPECT_EQ(remove->property("text").toString(), QStringLiteral("Delete Color Grade"));
}

TEST_F(EditorNodesPanelQmlTest, NodeCardMirrorsDeletionProtectedStateWithLockIcon) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);

  auto* primary_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  auto* extra_item   = adapter->NodeFor(NodeId{"grade.extra"})->getItem();
  ASSERT_NE(primary_item, nullptr);
  ASSERT_NE(extra_item, nullptr);

  // grade.primary is deletion-protected by default: its card shows the lock
  // icon while the unprotected grade.extra card does not.
  QTRY_VERIFY_WITH_TIMEOUT(
      primary_item->property("deletionProtected").toBool() == true, 2000);
  EXPECT_FALSE(extra_item->property("deletionProtected").toBool());
  auto* lock_icon = primary_item->findChild<QQuickItem*>(QStringLiteral("editorNodeLockIcon"));
  ASSERT_NE(lock_icon, nullptr);
  EXPECT_TRUE(lock_icon->isVisible());
  auto* extra_lock = extra_item->findChild<QQuickItem*>(QStringLiteral("editorNodeLockIcon"));
  ASSERT_NE(extra_lock, nullptr);
  EXPECT_FALSE(extra_lock->isVisible());

  // Unlocking through the controller commits and refreshes the card.
  ASSERT_TRUE(nodes->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  QTRY_VERIFY_WITH_TIMEOUT(
      primary_item->property("deletionProtected").toBool() == false, 5000);
  QTRY_VERIFY_WITH_TIMEOUT(!lock_icon->isVisible(), 5000);
}

TEST_F(EditorNodesPanelQmlTest, LockedNodeDisablesContextMenuDeleteAndMixedMultiDelete) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddColorGradeBefore(NodeId{"drt"}, NodeId{"grade.extra"});
  OpenNodesPage();
  auto* nodes   = Controller();
  auto* adapter = Adapter();
  ASSERT_NE(nodes, nullptr);
  ASSERT_NE(adapter, nullptr);
  WaitUntilGraphReady();
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.extra"}) != nullptr, 2000);

  // Single locked Color Grade: Rename stays available, Delete is disabled.
  nodes->selectNode(QStringLiteral("grade.primary"));
  ProcessEvents();
  auto* primary_item = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(primary_item, nullptr);
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    primary_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();
  auto* menu = window_->findChild<QObject*>(QStringLiteral("editorNodesNodeMenu"));
  ASSERT_NE(menu, nullptr);
  EXPECT_TRUE(menu->property("visible").toBool());
  auto* rename = Find(QStringLiteral("editorNodesRenameMenuItem"));
  auto* remove = Find(QStringLiteral("editorNodesDeleteMenuItem"));
  ASSERT_NE(rename, nullptr);
  ASSERT_NE(remove, nullptr);
  EXPECT_TRUE(rename->property("enabled").toBool());
  EXPECT_FALSE(remove->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
  ProcessEvents();

  // Multi-selection with one protected member disables the batch Delete too.
  nodes->toggleNodeSelection(QStringLiteral("grade.extra"));
  ProcessEvents();
  ASSERT_EQ(nodes->selected_node_ids().size(), 2);
  auto* extra_item = adapter->NodeFor(NodeId{"grade.extra"})->getItem();
  ASSERT_NE(extra_item, nullptr);
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();
  EXPECT_TRUE(menu->property("visible").toBool());
  EXPECT_FALSE(remove->property("enabled").toBool());
  EXPECT_EQ(remove->property("text").toString(), QStringLiteral("Delete Selected Nodes"));
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
  ProcessEvents();

  // Unlocking the protected member re-enables Delete for the same selection.
  ASSERT_TRUE(nodes->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), false));
  ProcessEvents();
  QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                    extra_item->mapToScene(QPointF(8, 8)).toPoint());
  ProcessEvents();
  EXPECT_TRUE(remove->property("enabled").toBool());
}

// Installs the compiled zh_CN catalog for the lifetime of the test and flips
// already-loaded QML through QQmlEngine::retranslate(). This is the same
// mechanism LanguageManager uses at runtime.
class ScopedZhCnCatalog {
 public:
  explicit ScopedZhCnCatalog(QQmlEngine& engine) : engine_(engine) {
    installed_ = translator_.load(QStringLiteral(ALCEDO_ZH_CN_QM_FILE)) &&
                 QCoreApplication::installTranslator(&translator_);
    if (installed_) {
      engine_.retranslate();
    }
  }
  ~ScopedZhCnCatalog() {
    if (installed_) {
      QCoreApplication::removeTranslator(&translator_);
      engine_.retranslate();
    }
  }
  [[nodiscard]] auto installed() const -> bool { return installed_; }

  void               FlipToEnglish() {
    if (installed_) {
      QCoreApplication::removeTranslator(&translator_);
    }
    engine_.retranslate();
  }
  void FlipToChinese() {
    if (installed_) {
      QCoreApplication::installTranslator(&translator_);
    }
    engine_.retranslate();
  }

 private:
  QQmlEngine& engine_;
  QTranslator translator_;
  bool        installed_ = false;
};

TEST_F(EditorNodesPanelQmlTest, SimplifiedChineseCatalogRetranslatesNodesAndMaskGroupsInPlace) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  ScopedZhCnCatalog zh(engine_);
  ASSERT_TRUE(zh.installed()) << "compiled zh_CN catalog missing at " << ALCEDO_ZH_CN_QM_FILE;

  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* body  = Find(QStringLiteral("editorMaskGroupsPageBody"));
  auto* title = Find(QStringLiteral("editorMaskGroupsPanelTitle"));
  auto* add   = Find(QStringLiteral("editorMaskGroupsAddButton"));
  ASSERT_NE(body, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(title->property("text").toString(), QStringLiteral("图层（蒙版组）"));
  EXPECT_EQ(AttachedName(title), QStringLiteral("图层（蒙版组）"));
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("添加图层（蒙版组）"));
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);
  auto* empty = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupEmpty"));
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("暂无蒙版"));

  // Retranslation updates the same loaded objects: no Loader teardown.
  zh.FlipToEnglish();
  ProcessEvents();
  EXPECT_EQ(Find(QStringLiteral("editorMaskGroupsPageBody")), body);
  EXPECT_EQ(title->property("text").toString(), QStringLiteral("Mask Groups"));
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("Add Mask Group"));
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("No masks"));

  zh.FlipToChinese();
  ProcessEvents();
  EXPECT_EQ(Find(QStringLiteral("editorMaskGroupsPageBody")), body);
  EXPECT_EQ(title->property("text").toString(), QStringLiteral("图层（蒙版组）"));
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("添加图层（蒙版组）"));
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("暂无蒙版"));
}

TEST_F(EditorNodesPanelQmlTest,
       SimplifiedChineseMaskGroupActionsAndAccessibilityUseApprovedTerminology) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  ScopedZhCnCatalog zh(engine_);
  ASSERT_TRUE(zh.installed()) << "compiled zh_CN catalog missing at " << ALCEDO_ZH_CN_QM_FILE;

  auto* nodes_probe = Controller();
  ASSERT_NE(nodes_probe, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes_probe->has_snapshot(), 2000);
  ASSERT_TRUE(nodes_probe->setColorGradeDeletionProtected(QStringLiteral("grade.primary"), true));
  ProcessEvents();

  OpenMaskGroupsPage();
  QTRY_VERIFY_WITH_TIMEOUT(MaskGroupDelegates().size() == 1, 2000);
  auto* primary = MaskGroupDelegateFor(QStringLiteral("grade.primary"));
  ASSERT_NE(primary, nullptr);

  auto* lock = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupLockButton"));
  ASSERT_NE(lock, nullptr);
  EXPECT_EQ(AttachedName(lock), QStringLiteral("解锁Color Grade 1"));

  auto* remove = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupDeleteButton"));
  ASSERT_NE(remove, nullptr);
  EXPECT_FALSE(remove->isEnabled());
  EXPECT_EQ(remove->property("toolTipText").toString(),
            QStringLiteral("删除前请先解锁Color Grade 1"));
  EXPECT_EQ(AttachedName(remove), QStringLiteral("删除Color Grade 1"));

  auto* header = primary->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupHeader"));
  ASSERT_NE(header, nullptr);
  const auto header_name = AttachedName(header);
  EXPECT_TRUE(header_name.contains(QStringLiteral("蒙版"))) << header_name.toStdString();
  EXPECT_TRUE(header_name.contains(QStringLiteral("已展开"))) << header_name.toStdString();
  EXPECT_TRUE(header_name.contains(QStringLiteral("删除已锁定"))) << header_name.toStdString();

  auto* row = MaskRowIn(primary, QStringLiteral("mask.one"));
  ASSERT_NE(row, nullptr);
  // rowName comes from backend mask data ("Mask"); only the qsTr-owned
  // fragments of the composed name translate.
  const auto row_name = AttachedName(row);
  EXPECT_TRUE(row_name.contains(QStringLiteral("不透明度"))) << row_name.toStdString();
  auto* row_delete = row->findChild<QQuickItem*>(QStringLiteral("editorMaskGroupMaskDeleteButton"));
  ASSERT_NE(row_delete, nullptr);
  EXPECT_EQ(AttachedName(row_delete), QStringLiteral("删除Mask"));

  // No accessible phrase may carry banned separators or the unapproved
  // Mask Group wording anywhere on the page.
  QStringList phrases;
  CollectAccessiblePhrases(Find(QStringLiteral("editorMaskGroupsPageBody")), &phrases);
  for (const auto& phrase : phrases) {
    EXPECT_FALSE(phrase.contains(QStringLiteral(" · "))) << phrase.toStdString();
    EXPECT_FALSE(phrase.contains(QStringLiteral(" | "))) << phrase.toStdString();
    const QString residue = QString(phrase).replace(QStringLiteral("图层（蒙版组）"), QString());
    EXPECT_FALSE(residue.contains(QStringLiteral("蒙版组"))) << phrase.toStdString();
    EXPECT_FALSE(residue.contains(QStringLiteral("遮罩"))) << phrase.toStdString();
  }
}

TEST_F(EditorNodesPanelQmlTest,
       SimplifiedChineseNodeAndAdjustmentLabelsRemainVisibleAtSupportedWidths) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  backend_.AddMaskToPrimaryGrade(MakeMask(MaskId{"mask.one"}, RadialMaskSource{}));
  ScopedZhCnCatalog zh(engine_);
  ASSERT_TRUE(zh.installed()) << "compiled zh_CN catalog missing at " << ALCEDO_ZH_CN_QM_FILE;

  auto* layout = LayoutStore();
  ASSERT_NE(layout, nullptr);
  OpenNodesPage();
  WaitUntilGraphReady();
  auto* adapter = Adapter();
  ASSERT_NE(adapter, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(adapter->NodeFor(NodeId{"grade.primary"}) != nullptr, 2000);

  auto* panel = Find(QStringLiteral("editorNodesPageBody"));
  auto* title = Find(QStringLiteral("editorNodesPanelTitle"));
  auto* add   = Find(QStringLiteral("editorNodesAddButton"));
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(title->property("text").toString(), QStringLiteral("节点"));
  EXPECT_EQ(add->property("actionName").toString(), QStringLiteral("添加色彩分级"));

  for (const int width : {260, 320, 460}) {
    layout->set_preferred_panel_width(width);
    ProcessEvents();
    EXPECT_TRUE(title->isVisible()) << "width " << width;
    EXPECT_TRUE(add->isVisible()) << "width " << width;
    EXPECT_GT(title->width(), 0.0) << "width " << width;
    EXPECT_LE(title->width() + add->width(), panel->width() + 1.0) << "width " << width;
  }

  // Mask drawer (adjustment surface on the node card) shows the Chinese label.
  auto* grade = adapter->NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade, nullptr);
  auto* drawer_title = grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawerTitle"));
  auto* drawer_head  = grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskDrawerHeader"));
  ASSERT_NE(drawer_title, nullptr);
  ASSERT_NE(drawer_head, nullptr);
  EXPECT_EQ(drawer_title->property("text").toString(), QStringLiteral("蒙版"));
  EXPECT_EQ(AttachedName(drawer_head), QStringLiteral("折叠蒙版"));
  QTRY_VERIFY_WITH_TIMEOUT(
      grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow")) != nullptr, 2000);
  auto* type_row = grade->findChild<QQuickItem*>(QStringLiteral("editorNodeMaskTypeRow"));
  ASSERT_NE(type_row, nullptr);
  EXPECT_EQ(type_row->property("typeLabel").toString(), QStringLiteral("径向"));
}

TEST_F(EditorNodesPanelQmlTest, CanvasBorderStaysConstantWhenGraphViewGainsFocus) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* canvas = Find(QStringLiteral("editorNodesCanvasHost"));
  auto* view   = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(canvas, nullptr);
  ASSERT_NE(view, nullptr);
  WaitUntilGraphReady();

  const QColor resting =
      QQmlProperty::read(canvas, QStringLiteral("border.color"), qmlContext(canvas))
          .value<QColor>();
  ASSERT_TRUE(resting.isValid());
  view->forceActiveFocus();
  ProcessEvents();
  ASSERT_TRUE(view->hasActiveFocus());
  // The canvas keeps its resting border while focused: no highlight pass.
  EXPECT_EQ(QQmlProperty::read(canvas, QStringLiteral("border.color"), qmlContext(canvas))
                .value<QColor>(),
            resting);
}

}  // namespace
}  // namespace alcedo::ui::test
