//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>
#include <QuickQanava>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "app/editor_node_graph_draft.hpp"
#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_model.hpp"
#include "qanEdge.h"
#include "qanEdgeItem.h"
#include "qanGraph.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "ui/alcedo_main/app_theme.hpp"

#include <algorithm>

namespace {

constexpr char kGraphHarnessQml[] = R"qml(
import QtQuick
import QtQuick.Controls
import QuickQanava 2.0 as Qan

ApplicationWindow {
    id: root
    objectName: "alcedoQanPerformanceHarness"
    width: 960
    height: 640
    visible: true

    Component {
        id: graphViewComponent

        Qan.GraphView {
            id: graphView
            objectName: "qanGraphView"
            anchors.fill: parent
            navigable: false

            graph: Qan.Graph {
                id: graphTopology
                objectName: "qanGraph"
            }
        }
    }

    Loader {
        id: graphLoader
        objectName: "graphLoader"
        anchors.fill: parent
        active: true
        asynchronous: false
        sourceComponent: graphViewComponent
    }
}
)qml";

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto QmlFileUrl(const char* file_name) -> QUrl {
  return QUrl::fromLocalFile(SrcQmlDir() + QLatin1Char('/') + QLatin1String(file_name));
}

void SetBasicStyle() {
  static std::once_flag style_once;
  std::call_once(style_once, [] { QQuickStyle::setStyle(QStringLiteral("Basic")); });
}

bool WaitFor(const std::function<bool()>& predicate, int timeout_ms = 2000) {
  QDeadlineTimer deadline{timeout_ms};
  while (!predicate() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  return predicate();
}

class QanHarness final {
 public:
  QanHarness() {
    QObject::connect(&engine_, &QQmlApplicationEngine::warnings, &engine_,
                     [this](const QList<QQmlError>& errors) {
                       for (const auto& error : errors) {
                         warnings_.append(error.toString());
                       }
                     });

    SetBasicStyle();
    alcedo::ui::AppTheme::Instance().setReduceMotion(true);
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(SrcQmlDir());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                              &alcedo::ui::AppTheme::Instance());
    QuickQanava::initialize(&engine_);
    engine_.loadData(QByteArray{kGraphHarnessQml},
                     QUrl(QStringLiteral("file:///AlcedoQanGraphPerformanceHarness.qml")));

    if (!engine_.rootObjects().isEmpty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().constFirst());
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  }

  [[nodiscard]] auto window() const -> QQuickWindow* { return window_; }
  [[nodiscard]] auto warnings() const -> const QStringList& { return warnings_; }
  [[nodiscard]] auto Graph() const -> qan::Graph* {
    return window_ == nullptr ? nullptr : window_->findChild<qan::Graph*>("qanGraph");
  }
  [[nodiscard]] auto Loader() const -> QObject* {
    return window_ == nullptr ? nullptr : window_->findChild<QObject*>("graphLoader");
  }

 private:
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

auto WarningText(const QStringList& warnings) -> std::string {
  return warnings.join(QLatin1Char('\n')).toStdString();
}

void AttachAlcedoDelegates(alcedo::ui::AlcedoQanGraph& adapter, qan::Graph* graph) {
  adapter.set_color_grade_delegate_url(QmlFileUrl("EditorNodeDelegate.qml"));
  adapter.set_endpoint_delegate_url(QmlFileUrl("EditorEndpointNodeDelegate.qml"));
  adapter.set_port_delegate_url(QmlFileUrl("EditorNodePortDelegate.qml"));
  adapter.set_port_dock_delegate_url(QmlFileUrl("EditorNodePortDock.qml"));
  adapter.set_edge_delegate_url(QmlFileUrl("EditorNodeEdgeDelegate.qml"));
  adapter.set_graph(graph);
}

auto MakeMask(const std::string& id, float center) -> alcedo::MaskModel {
  alcedo::MaskModel mask;
  mask.id           = alcedo::MaskId{id};
  mask.display_name = "Mask";
  alcedo::RadialMaskSource radial;
  radial.center_x = center;
  mask.source     = std::move(radial);
  return mask;
}

auto DocumentWithGrades(int grade_count, int masks_per_grade) -> alcedo::PipelineDocument {
  auto document = alcedo::CreateDefaultPipelineDocument();
  auto fill     = [&](alcedo::ColorGradeNodeModel* grade, const std::string& prefix) {
    if (grade == nullptr) {
      return;
    }
    for (int mask = 0; mask < masks_per_grade; ++mask) {
      grade->AddMask(MakeMask(prefix + "." + std::to_string(mask), 0.1f * static_cast<float>(mask)),
                     static_cast<std::size_t>(mask));
    }
  };
  fill(document.PrimaryGrade(), "mask.primary");
  for (int extra = 0; extra < grade_count - 1; ++extra) {
    const auto id = alcedo::NodeId{"grade.g" + std::to_string(extra)};
    EXPECT_TRUE(alcedo::AddCleanColorGrade(document, alcedo::NodeId{"drt"}, id).empty());
    fill(dynamic_cast<alcedo::ColorGradeNodeModel*>(document.Graph().FindNode(id)),
         "mask.g" + std::to_string(extra));
  }
  return document;
}

auto BoundIdentity() -> alcedo::EditorNodeGraphDraftIdentity {
  alcedo::EditorNodeGraphDraftIdentity identity;
  identity.element_id          = 8;
  identity.image_id            = 9;
  identity.version_id          = "v1";
  identity.session_generation  = 4;
  identity.projection_revision = 1;
  identity.topology_revision   = 1;
  return identity;
}

auto LiveNodeDelegateCount(const alcedo::ui::AlcedoQanGraph&        adapter,
                           const alcedo::EditorNodeGraphSnapshot& snapshot) -> int {
  int count = 0;
  for (const auto& node : snapshot.nodes) {
    auto* qan_node = adapter.NodeFor(node.node_id);
    if (qan_node != nullptr && qan_node->getItem() != nullptr) {
      ++count;
    }
  }
  return count;
}

auto LiveEdgeDelegateCount(const alcedo::ui::AlcedoQanGraph&        adapter,
                           const alcedo::EditorNodeGraphSnapshot& snapshot) -> int {
  int count = 0;
  for (const auto& edge : snapshot.edges) {
    auto* qan_edge = adapter.EdgeFor(edge);
    if (qan_edge != nullptr && qan_edge->getItem() != nullptr) {
      ++count;
    }
  }
  return count;
}

}  // namespace

namespace alcedo {

class AlcedoQanGraphPerformance : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    harness_ = std::make_unique<QanHarness>();
    ASSERT_NE(harness_->window(), nullptr) << WarningText(harness_->warnings());
    ASSERT_TRUE(harness_->warnings().isEmpty()) << WarningText(harness_->warnings());
  }

  static void TearDownTestSuite() { harness_.reset(); }

  void        SetUp() override {
    ASSERT_NE(harness_, nullptr);
    auto* loader = harness_->Loader();
    ASSERT_NE(loader, nullptr);
    ASSERT_TRUE(loader->setProperty("active", false));
    ASSERT_TRUE(WaitFor([] { return harness_->Graph() == nullptr; }));
    ASSERT_TRUE(loader->setProperty("active", true));
    ASSERT_TRUE(WaitFor([] { return harness_->Graph() != nullptr; }));
  }

  static std::unique_ptr<QanHarness> harness_;
};

std::unique_ptr<QanHarness> AlcedoQanGraphPerformance::harness_;

TEST_F(AlcedoQanGraphPerformance, DefaultGraphApplyCreatesThreeNodeDelegatesAndTwoEdges) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  QElapsedTimer timer;
  timer.start();
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  const auto result   = adapter.ApplySnapshot(snapshot);
  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  ASSERT_TRUE(WaitFor([&] { return LiveNodeDelegateCount(adapter, snapshot) == 3; }));
  const auto apply_ms = static_cast<int>(timer.elapsed());
  RecordProperty("default_graph_apply_ms", apply_ms);
  RecordProperty("default_graph_node_delegates", LiveNodeDelegateCount(adapter, snapshot));
  RecordProperty("default_graph_edge_delegates", LiveEdgeDelegateCount(adapter, snapshot));

  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_EQ(adapter.graph()->getNodeCount(), 3);
  EXPECT_EQ(adapter.graph()->get_edge_count(), 2);
  EXPECT_EQ(LiveNodeDelegateCount(adapter, snapshot), 3);
  EXPECT_EQ(LiveEdgeDelegateCount(adapter, snapshot), 2);
}

TEST_F(AlcedoQanGraphPerformance, ThirtyTwoGradeGraphApplyRecordsDelegateCountsAndApplyTime) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = DocumentWithGrades(32, 8);
  EXPECT_EQ(document.Graph().NodeCount(), 34u);
  const auto snapshot = EditorNodeGraphProjection::Build(document, 2, 1, 1);
  QElapsedTimer timer;
  timer.start();
  const auto result = adapter.ApplySnapshot(snapshot);
  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  ASSERT_TRUE(WaitFor([&] { return LiveNodeDelegateCount(adapter, snapshot) == 34; }, 15000));
  const auto apply_ms = static_cast<int>(timer.elapsed());
  RecordProperty("thirty_two_grade_apply_ms", apply_ms);
  RecordProperty("thirty_two_grade_node_delegates", LiveNodeDelegateCount(adapter, snapshot));
  RecordProperty("thirty_two_grade_edge_delegates", LiveEdgeDelegateCount(adapter, snapshot));

  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_EQ(adapter.graph()->getNodeCount(), 34);
  EXPECT_EQ(adapter.graph()->get_edge_count(), 33);
  EXPECT_EQ(LiveNodeDelegateCount(adapter, snapshot), 34);
  EXPECT_EQ(LiveEdgeDelegateCount(adapter, snapshot), 33);
}

TEST_F(AlcedoQanGraphPerformance, OneHundredSelectionsStayWithinOneHundredMillisecondsEach) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 3, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);
  ASSERT_TRUE(WaitFor([&] { return adapter.NodeFor(NodeId{"grade.primary"}) != nullptr; }));
  const NodeId ids[] = {NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}};
  QElapsedTimer total;
  total.start();
  qint64 max_ms = 0;
  for (int step = 0; step < 100; ++step) {
    QElapsedTimer step_timer;
    step_timer.start();
    adapter.ApplyProductSelection(ids[step % 3]);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    max_ms = std::max(max_ms, step_timer.elapsed());
  }
  RecordProperty("one_hundred_selection_total_ms", static_cast<int>(total.elapsed()));
  RecordProperty("one_hundred_selection_max_ms", static_cast<int>(max_ms));
  EXPECT_EQ(adapter.topology_replace_count(), 1);
  EXPECT_LT(max_ms, 100);
}

TEST_F(AlcedoQanGraphPerformance, OpeningAndClosingAManyMaskNodeKeepsOwnerIdentity) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = DocumentWithGrades(1, 16);
  const auto snapshot = EditorNodeGraphProjection::Build(document, 4, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);
  ASSERT_TRUE(WaitFor([&] { return adapter.NodeFor(NodeId{"grade.primary"}) != nullptr; }));
  QPointer<qan::Node> grade = adapter.NodeFor(NodeId{"grade.primary"});
  QElapsedTimer timer;
  timer.start();
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  ASSERT_TRUE(WaitFor([&] {
    auto* node = adapter.NodeFor(NodeId{"grade.primary"});
    auto* item = node == nullptr ? nullptr : node->getItem();
    return item != nullptr && !item->property("drawerOpen").toBool();
  }));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, true);
  ASSERT_TRUE(WaitFor([&] {
    auto* node = adapter.NodeFor(NodeId{"grade.primary"});
    auto* item = node == nullptr ? nullptr : node->getItem();
    return item != nullptr && item->property("drawerOpen").toBool();
  }));
  const auto fold_ms = static_cast<int>(timer.elapsed());
  RecordProperty("many_mask_drawer_fold_ms", fold_ms);
  EXPECT_EQ(grade.data(), adapter.NodeFor(NodeId{"grade.primary"}));
  EXPECT_EQ(adapter.topology_replace_count(), 1);
  EXPECT_LT(fold_ms, 100);
}

TEST_F(AlcedoQanGraphPerformance, OneHundredAcceptedDraftConnectsRetainQanIdentities) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, BoundIdentity());
  const auto snapshot = EditorNodeGraphProjection::Build(document, 4, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);
  ASSERT_TRUE(WaitFor([&] { return adapter.NodeFor(NodeId{"grade.primary"}) != nullptr; }));
  QPointer<qan::Node> develop = adapter.NodeFor(NodeId{"develop"});
  QPointer<qan::Node> primary = adapter.NodeFor(NodeId{"grade.primary"});
  QPointer<qan::Node> drt     = adapter.NodeFor(NodeId{"drt"});
  const auto replace_count    = adapter.topology_replace_count();
  auto       add              = draft.AddColorGrade(NodeId{"grade.extra"});
  ASSERT_TRUE(add.succeeded) << add.error;
  const auto add_visual = adapter.ApplyMutation(add);
  ASSERT_TRUE(add_visual.succeeded) << add_visual.error.toStdString();
  ASSERT_TRUE(WaitFor([&] { return adapter.NodeFor(NodeId{"grade.extra"}) != nullptr; }));

  int         changed_edges = 0;
  QElapsedTimer timer;
  timer.start();
  const NodeId develop_id{"develop"};
  const NodeId extra_id{"grade.extra"};
  const NodeId primary_id{"grade.primary"};
  for (int step = 0; step < 50; ++step) {
    auto first = draft.Connect(develop_id, extra_id);
    ASSERT_TRUE(first.succeeded) << first.error;
    changed_edges += static_cast<int>(first.removed_edges.size() + first.inserted_edges.size());
    EXPECT_LE(first.removed_edges.size(), 2u);
    EXPECT_LE(first.inserted_edges.size(), 1u);
    const auto first_visual = adapter.ApplyMutation(first);
    ASSERT_TRUE(first_visual.succeeded) << first_visual.error.toStdString();
    auto second = draft.Connect(develop_id, primary_id);
    ASSERT_TRUE(second.succeeded) << second.error;
    EXPECT_LE(second.removed_edges.size(), 2u);
    EXPECT_LE(second.inserted_edges.size(), 1u);
    changed_edges += static_cast<int>(second.removed_edges.size() + second.inserted_edges.size());
    const auto second_visual = adapter.ApplyMutation(second);
    ASSERT_TRUE(second_visual.succeeded) << second_visual.error.toStdString();
  }
  const auto connect_ms = static_cast<int>(timer.elapsed());
  RecordProperty("one_hundred_draft_connect_ms", connect_ms);
  RecordProperty("one_hundred_draft_connect_changed_edges", changed_edges);
  EXPECT_EQ(adapter.topology_replace_count(), replace_count);
  EXPECT_EQ(develop.data(), adapter.NodeFor(NodeId{"develop"}));
  EXPECT_EQ(primary.data(), adapter.NodeFor(NodeId{"grade.primary"}));
  EXPECT_EQ(drt.data(), adapter.NodeFor(NodeId{"drt"}));
  EXPECT_FALSE(develop.isNull());
  EXPECT_NE(adapter.NodeFor(NodeId{"grade.extra"}), nullptr);
}

}  // namespace alcedo
