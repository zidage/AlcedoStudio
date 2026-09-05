//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QUrl>
#include <QuickQanava>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_model.hpp"
#include "qanConnector.h"
#include "qanEdge.h"
#include "qanEdgeItem.h"
#include "qanGraph.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "qanPortItem.h"
#include "ui/alcedo_main/app_theme.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace {

constexpr char kGraphHarnessQml[] = R"qml(
import QtQuick
import QtQuick.Controls
import QuickQanava 2.0 as Qan

ApplicationWindow {
    id: root
    objectName: "alcedoQanHarness"
    width: 640
    height: 480
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

auto           SrcQmlDir() -> QString {
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

bool WaitFor(const std::function<bool()>& predicate, int timeout_ms = 1000) {
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
                     QUrl(QStringLiteral("file:///AlcedoQanGraphHarness.qml")));

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

auto MakeMask(alcedo::MaskId id, alcedo::MaskSource source) -> alcedo::MaskModel {
  alcedo::MaskModel mask;
  mask.id           = std::move(id);
  mask.display_name = "Mask";
  mask.source       = std::move(source);
  return mask;
}

class FailAfterInsertsGraph final : public alcedo::ui::AlcedoQanGraph {
 public:
  void FailAfterSuccessfulInserts(int count) { remaining_success_ = count; }

 protected:
  auto InsertQanNode(qan::Graph& graph, const alcedo::EditorNodeProjection& node)
      -> qan::Node* override {
    if (remaining_success_.has_value()) {
      if (*remaining_success_ == 0) {
        remaining_success_.reset();
        return nullptr;
      }
      --*remaining_success_;
    }
    return AlcedoQanGraph::InsertQanNode(graph, node);
  }

 private:
  std::optional<int> remaining_success_;
};

enum class QanOperation : std::size_t {
  InsertNode = 0,
  InsertPort,
  InsertEdge,
  BindEdge,
  RemoveEdge,
  RemovePort,
  RemoveNode,
};

class FailureInjectingQanGraph final : public alcedo::ui::AlcedoQanGraph {
 public:
  void FailOnNext(QanOperation operation) { FailOnCall(operation, 1); }

  void FailOnCall(QanOperation operation, int call_number) {
    failure_calls_.fill(std::nullopt);
    failure_calls_[static_cast<std::size_t>(operation)] = call_number;
    operation_calls_.fill(0);
  }

  void FailOnCalls(QanOperation first_operation, int first_call, QanOperation second_operation,
                   int second_call) {
    failure_calls_.fill(std::nullopt);
    failure_calls_[static_cast<std::size_t>(first_operation)]  = first_call;
    failure_calls_[static_cast<std::size_t>(second_operation)] = second_call;
    operation_calls_.fill(0);
  }

 protected:
  auto InsertQanNode(qan::Graph& graph, const alcedo::EditorNodeProjection& node)
      -> qan::Node* override {
    if (ShouldFail(QanOperation::InsertNode)) {
      return nullptr;
    }
    return AlcedoQanGraph::InsertQanNode(graph, node);
  }

  auto InsertQanPort(qan::Graph& graph, qan::Node& node, bool is_input, const QString& port_id)
      -> qan::PortItem* override {
    if (ShouldFail(QanOperation::InsertPort)) {
      return nullptr;
    }
    return AlcedoQanGraph::InsertQanPort(graph, node, is_input, port_id);
  }

  auto InsertQanEdge(qan::Graph& graph, qan::Node& source, qan::Node& destination,
                     QQmlComponent* component) -> qan::Edge* override {
    if (ShouldFail(QanOperation::InsertEdge)) {
      return nullptr;
    }
    return AlcedoQanGraph::InsertQanEdge(graph, source, destination, component);
  }

  auto BindQanEdge(qan::Graph& graph, qan::Edge& edge, qan::PortItem& source,
                   qan::PortItem& destination) -> bool override {
    if (ShouldFail(QanOperation::BindEdge)) {
      return false;
    }
    return AlcedoQanGraph::BindQanEdge(graph, edge, source, destination);
  }

  auto RemoveQanEdge(qan::Graph& graph, qan::Edge& edge) -> bool override {
    if (ShouldFail(QanOperation::RemoveEdge)) {
      return false;
    }
    return AlcedoQanGraph::RemoveQanEdge(graph, edge);
  }

  auto RemoveQanPort(qan::Graph& graph, qan::Node& node, qan::PortItem& port) -> bool override {
    if (ShouldFail(QanOperation::RemovePort)) {
      return false;
    }
    return AlcedoQanGraph::RemoveQanPort(graph, node, port);
  }

  auto RemoveQanNode(qan::Graph& graph, qan::Node& node) -> bool override {
    if (ShouldFail(QanOperation::RemoveNode)) {
      return false;
    }
    return AlcedoQanGraph::RemoveQanNode(graph, node);
  }

 private:
  [[nodiscard]] auto ShouldFail(QanOperation operation) -> bool {
    const auto index = static_cast<std::size_t>(operation);
    ++operation_calls_[index];
    if (!failure_calls_[index].has_value() || operation_calls_[index] != *failure_calls_[index]) {
      return false;
    }
    failure_calls_[index].reset();
    return true;
  }

  std::array<std::optional<int>, 7> failure_calls_{};
  std::array<int, 7>                operation_calls_{};
};

}  // namespace

namespace alcedo {

constexpr auto kImagePort = [] { return PortId{"image"}; };

auto ExpectLiveBackbone(const ui::AlcedoQanGraph& adapter, const EditorNodeGraphSnapshot& snapshot)
    -> void {
  ASSERT_EQ(adapter.graph() == nullptr ? 0 : adapter.graph()->getNodeCount(),
            static_cast<int>(snapshot.nodes.size()));
  for (const auto& node : snapshot.nodes) {
    auto* qan_node = adapter.NodeFor(node.node_id);
    ASSERT_NE(qan_node, nullptr) << std::string{node.node_id.Value()};
    EXPECT_EQ(adapter.LiveNodeId(qan_node), node.node_id);
    EXPECT_EQ(qan_node->getLabel(),
              QString::fromUtf8(node.display_name.data(),
                                static_cast<qsizetype>(node.display_name.size())));
    EXPECT_NE(qan_node->getLabel().toStdString(), std::string{node.node_id.Value()});
    EXPECT_NE(qan_node->getItem(), nullptr);
  }

  const PortId image = kImagePort();
  EXPECT_EQ(adapter.InputPortFor(NodeId{"develop"}, image), nullptr);
  EXPECT_NE(adapter.OutputPortFor(NodeId{"develop"}, image), nullptr);
  EXPECT_EQ(adapter.OutputPortFor(NodeId{"drt"}, image), nullptr);
  EXPECT_NE(adapter.InputPortFor(NodeId{"drt"}, image), nullptr);

  for (const auto& edge : snapshot.edges) {
    auto* qan_edge = adapter.EdgeFor(edge);
    ASSERT_NE(qan_edge, nullptr);
    ASSERT_NE(qan_edge->getItem(), nullptr);
    auto* source_port = adapter.OutputPortFor(edge.source_node_id, edge.source_port_id);
    auto* dest_port   = adapter.InputPortFor(edge.destination_node_id, edge.destination_port_id);
    ASSERT_NE(source_port, nullptr);
    ASSERT_NE(dest_port, nullptr);
    EXPECT_EQ(source_port->getDockType(), qan::NodeItem::Dock::Bottom);
    EXPECT_EQ(source_port->getType(), qan::PortItem::Type::Out);
    EXPECT_EQ(dest_port->getDockType(), qan::NodeItem::Dock::Top);
    EXPECT_EQ(dest_port->getType(), qan::PortItem::Type::In);
    EXPECT_EQ(qan_edge->getItem()->getSourceItem(), source_port);
    EXPECT_EQ(qan_edge->getItem()->getDestinationItem(), dest_port);
    EXPECT_TRUE(qan_edge->getLabel().isEmpty());
  }
}

auto MakeAddedColorGradeMutation() -> EditorNodeGraphDraftMutation {
  auto document = CreateDefaultPipelineDocument();
  auto draft    = EditorNodeGraphDraft::FromDocument(document, {});
  return draft.AddColorGrade(NodeId{"grade.extra"});
}

auto MakeEdgeMutation(const EditorNodeEdgeProjection& first_removed,
                      const EditorNodeEdgeProjection& second_removed)
    -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation mutation;
  mutation.succeeded = true;
  mutation.removed_edges.push_back(first_removed);
  mutation.removed_edges.push_back(second_removed);
  mutation.inserted_edges.push_back(EditorNodeEdgeProjection{
      NodeId{"develop"}, PortId{"image"}, NodeId{"grade.extra.a"}, PortId{"image"}});
  mutation.inserted_edges.push_back(EditorNodeEdgeProjection{
      NodeId{"grade.extra.a"}, PortId{"image"}, NodeId{"grade.extra.b"}, PortId{"image"}});
  return mutation;
}

auto MakeExtraNode(const char* node_id) -> EditorNodeProjection {
  EditorNodeProjection node;
  node.node_id      = NodeId{node_id};
  node.node_kind    = EditorNodeKind::ColorGrade;
  node.display_name = "Detached Color Grade";
  return node;
}

void ExpectSelectedNode(const qan::Graph& graph, const qan::Node* expected) {
  const auto& selected = graph.getSelectedNodes();
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected.at(0).data(), expected);
}

void ExpectMappedEdges(const ui::AlcedoQanGraph&                    adapter,
                       const std::vector<EditorNodeEdgeProjection>& edges) {
  ASSERT_EQ(adapter.graph()->get_edge_count(), static_cast<int>(edges.size()));
  for (const auto& edge : edges) {
    auto* qan_edge = adapter.EdgeFor(edge);
    ASSERT_NE(qan_edge, nullptr);
    ASSERT_NE(qan_edge->getItem(), nullptr);
    EXPECT_EQ(qan_edge->getItem()->getSourceItem(),
              adapter.OutputPortFor(edge.source_node_id, edge.source_port_id));
    EXPECT_EQ(qan_edge->getItem()->getDestinationItem(),
              adapter.InputPortFor(edge.destination_node_id, edge.destination_port_id));
  }
}

class AlcedoQanGraph : public ::testing::Test {
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

std::unique_ptr<QanHarness> AlcedoQanGraph::harness_;

TEST_F(AlcedoQanGraph, MapsEachProjectedNodeIdToOneLiveQanNodeInTheCurrentGeneration) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 4, 2, 1);
  const auto result   = adapter.ApplySnapshot(snapshot);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_EQ(adapter.session_generation(), 4u);
  ExpectLiveBackbone(adapter, snapshot);
  EXPECT_EQ(adapter.LiveNodeId(nullptr), std::nullopt);
}

TEST_F(AlcedoQanGraph, BindsEachBackboneEdgeToTheMatchingTopAndBottomPorts) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  const auto snapshot = EditorNodeGraphProjection::Build(document, 1, 3, 2);
  const auto result   = adapter.ApplySnapshot(snapshot);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  ASSERT_EQ(snapshot.edges.size(), 3u);
  ExpectLiveBackbone(adapter, snapshot);
  EXPECT_EQ(adapter.OutputPortFor(NodeId{"grade.primary"}, PortId{"image"})->getId(),
            QStringLiteral("out:image"));
  EXPECT_EQ(adapter.InputPortFor(NodeId{"grade.second"}, PortId{"image"})->getId(),
            QStringLiteral("in:image"));
}

TEST_F(AlcedoQanGraph, InstallsFlushPortDockAndInvisibleSelectionDelegate) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 4, 2, 1);
  const auto result   = adapter.ApplySnapshot(snapshot);
  ASSERT_TRUE(result.succeeded) << result.error.toStdString();

  auto* graph = harness_->Graph();
  ASSERT_NE(graph, nullptr);

  const auto* dock_component = graph->property("horizontalDockDelegate").value<QQmlComponent*>();
  ASSERT_NE(dock_component, nullptr);
  EXPECT_TRUE(dock_component->url().toString().endsWith(QStringLiteral("EditorNodePortDock.qml")))
      << dock_component->url().toString().toStdString();

  const auto* selection_component = graph->property("selectionDelegate").value<QQmlComponent*>();
  ASSERT_NE(selection_component, nullptr);
  EXPECT_TRUE(selection_component->url().isEmpty())
      << "selection delegate must be the inline invisible Item, not "
      << selection_component->url().toString().toStdString();

  auto* out_port = adapter.OutputPortFor(NodeId{"develop"}, PortId{"image"});
  ASSERT_NE(out_port, nullptr);
  auto* dock = out_port->parentItem();
  ASSERT_NE(dock, nullptr);
  auto* node_item = adapter.NodeFor(NodeId{"develop"})->getItem();
  ASSERT_NE(node_item, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return std::abs(dock->y() - node_item->height()) < 0.5;
  })) << "output port dock must sit flush against the node bottom edge, dock y="
      << dock->y() << " node height=" << node_item->height();
}

TEST_F(AlcedoQanGraph, RenameUpdatesOneNodeLabelWithoutReplacingQanPrimitives) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto       document = CreateDefaultPipelineDocument();
  const auto before   = EditorNodeGraphProjection::Build(document, 6, 10, 4);
  ASSERT_TRUE(adapter.ApplySnapshot(before).succeeded) << "initial apply";

  QPointer<qan::Node> grade = adapter.NodeFor(NodeId{"grade.primary"});
  QPointer<qan::Edge> first = adapter.EdgeFor(before.edges.front());
  QPointer<qan::Edge> last  = adapter.EdgeFor(before.edges.back());
  ASSERT_FALSE(grade.isNull());

  ASSERT_TRUE(RenameColorGrade(document, NodeId{"grade.primary"}, "Look A").empty());
  const auto after  = EditorNodeGraphProjection::Build(document, 6, 11, 4);
  const auto result = adapter.ApplySnapshot(after);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  EXPECT_FALSE(result.rebuilt_topology);
  EXPECT_EQ(grade.data(), adapter.NodeFor(NodeId{"grade.primary"}));
  EXPECT_EQ(first.data(), adapter.EdgeFor(after.edges.front()));
  EXPECT_EQ(last.data(), adapter.EdgeFor(after.edges.back()));
  EXPECT_EQ(grade->getLabel(), QStringLiteral("Look A"));
  EXPECT_EQ(adapter.NodeProjection(NodeId{"grade.primary"})->display_name, "Look A");
  EXPECT_EQ(harness_->Graph()->getNodeCount(), 3);
}

TEST_F(AlcedoQanGraph, MaskKindChangeUpdatesOneNodeWithoutReplacingEdges) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 0);

  const auto before = EditorNodeGraphProjection::Build(document, 2, 5, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(before).succeeded) << "initial apply";
  QPointer<qan::Node> grade_node = adapter.NodeFor(NodeId{"grade.primary"});
  QPointer<qan::Edge> incoming   = adapter.EdgeFor(before.edges.front());
  QPointer<qan::Edge> outgoing   = adapter.EdgeFor(before.edges.back());

  grade->ReplaceMaskSource(MaskId{"mask.radial"}, BrushMaskSource{});
  const auto after  = EditorNodeGraphProjection::Build(document, 2, 6, 1);
  const auto result = adapter.ApplySnapshot(after);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  EXPECT_FALSE(result.rebuilt_topology);
  EXPECT_EQ(grade_node.data(), adapter.NodeFor(NodeId{"grade.primary"}));
  EXPECT_EQ(incoming.data(), adapter.EdgeFor(after.edges.front()));
  EXPECT_EQ(outgoing.data(), adapter.EdgeFor(after.edges.back()));
  ASSERT_NE(adapter.NodeProjection(NodeId{"grade.primary"}), nullptr);
  ASSERT_EQ(adapter.NodeProjection(NodeId{"grade.primary"})->masks.size(), 1u);
  EXPECT_EQ(adapter.NodeProjection(NodeId{"grade.primary"})->masks.front().source_kind,
            MaskSourceKind::Brush);
  EXPECT_EQ(harness_->Graph()->getNodeCount(), 3);
}

TEST_F(AlcedoQanGraph, RemovingAColorGradeReplacesQanPrimitivesAndHidesTheRemovedCard) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  const auto first = EditorNodeGraphProjection::Build(document, 9, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(first).succeeded) << "initial apply";

  QPointer<qan::Node> removed_node = adapter.NodeFor(NodeId{"grade.primary"});
  ASSERT_FALSE(removed_node.isNull());
  QPointer<QQuickItem> removed_item = removed_node->getItem();
  ASSERT_FALSE(removed_item.isNull());
  EXPECT_TRUE(removed_item->isVisible());

  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  const auto second = EditorNodeGraphProjection::Build(document, 9, 2, 2);
  const auto result = adapter.ApplySnapshot(second);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_TRUE(removed_node.isNull());
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), nullptr);
  if (!removed_item.isNull()) {
    EXPECT_FALSE(removed_item->isVisible());
    EXPECT_EQ(removed_item->parentItem(), nullptr);
  }
  ExpectLiveBackbone(adapter, second);
}

TEST_F(AlcedoQanGraph, VersionReplacementRemovesOldPrimitivesAndReverseMapEntries) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto first = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 8, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(first).succeeded) << "initial apply";

  QPointer<qan::Node> old_develop = adapter.NodeFor(NodeId{"develop"});
  QPointer<qan::Node> old_grade   = adapter.NodeFor(NodeId{"grade.primary"});
  QPointer<qan::Edge> old_edge    = adapter.EdgeFor(first.edges.front());
  ASSERT_FALSE(old_develop.isNull());
  bool maps_cleared_before_destroy = false;
  QObject::connect(old_develop.data(), &QObject::destroyed, &adapter, [&]() {
    maps_cleared_before_destroy = !adapter.LiveNodeId(old_develop.data()).has_value();
  });

  auto next_document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(next_document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  const auto second = EditorNodeGraphProjection::Build(next_document, 8, 4, 2);
  const auto result = adapter.ApplySnapshot(second);

  ASSERT_TRUE(result.succeeded) << result.error.toStdString();
  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_TRUE(maps_cleared_before_destroy);
  EXPECT_TRUE(old_develop.isNull());
  EXPECT_TRUE(old_grade.isNull());
  EXPECT_TRUE(old_edge.isNull());
  EXPECT_EQ(adapter.LiveNodeId(old_develop.data()), std::nullopt);
  ExpectLiveBackbone(adapter, second);
  EXPECT_NE(adapter.NodeFor(NodeId{"develop"}), old_develop.data());
}

TEST_F(AlcedoQanGraph, StalePrimitiveCannotSelectOrEditTheNewDocument) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto current = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 12, 3, 2);
  ASSERT_TRUE(adapter.ApplySnapshot(current).succeeded) << "initial apply";
  QPointer<qan::Node> live_grade = adapter.NodeFor(NodeId{"grade.primary"});
  ASSERT_FALSE(live_grade.isNull());

  const auto stale = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 11, 9, 9);
  const auto stale_result = adapter.ApplySnapshot(stale);
  EXPECT_FALSE(stale_result.succeeded);
  EXPECT_EQ(stale_result.error, QStringLiteral("snapshot session generation is stale"));
  EXPECT_EQ(adapter.session_generation(), 12u);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), live_grade.data());
  EXPECT_EQ(adapter.LiveNodeId(live_grade.data()), NodeId{"grade.primary"});
  EXPECT_EQ(harness_->Graph()->getNodeCount(), 3);

  auto next_document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(RenameColorGrade(next_document, NodeId{"grade.primary"}, "Version B").empty());
  const auto next = EditorNodeGraphProjection::Build(next_document, 13, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(next).succeeded) << "generation replacement";
  EXPECT_TRUE(live_grade.isNull());
  EXPECT_EQ(adapter.LiveNodeId(live_grade.data()), std::nullopt);
  ASSERT_NE(adapter.NodeFor(NodeId{"grade.primary"}), nullptr);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"})->getLabel(), QStringLiteral("Version B"));
}

TEST_F(AlcedoQanGraph, AdapterInsertFailureRestoresThePriorCompleteQanProjection) {
  FailAfterInsertsGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 3, 7, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ExpectLiveBackbone(adapter, prior);

  auto next_document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(next_document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  const auto next = EditorNodeGraphProjection::Build(next_document, 3, 8, 2);
  adapter.FailAfterSuccessfulInserts(1);
  const auto result = adapter.ApplySnapshot(next);

  EXPECT_FALSE(result.succeeded);
  EXPECT_TRUE(result.rebuilt_topology);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan node creation failed")), -1);
  EXPECT_EQ(adapter.session_generation(), 3u);
  EXPECT_EQ(adapter.topology_revision(), 1u);
  EXPECT_EQ(adapter.projection_revision(), 7u);
  ExpectLiveBackbone(adapter, prior);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.second"}), nullptr);
}

TEST_F(AlcedoQanGraph, FailedNodeInsertionPreservesUnrelatedIdentitySelectionAndLayout) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 20, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";

  auto* develop = adapter.NodeFor(NodeId{"develop"});
  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  auto* drt     = adapter.NodeFor(NodeId{"drt"});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(drt, nullptr);
  QPointer<qan::Edge> first_edge = adapter.EdgeFor(prior.edges.front());
  QPointer<qan::Edge> last_edge  = adapter.EdgeFor(prior.edges.back());
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(131, 247));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  adapter.FailOnNext(QanOperation::InsertNode);
  const auto result = adapter.ApplyMutation(MakeAddedColorGradeMutation());

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan node creation failed")), -1);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"}), develop);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(adapter.NodeFor(NodeId{"drt"}), drt);
  EXPECT_EQ(adapter.EdgeFor(prior.edges.front()), first_edge.data());
  EXPECT_EQ(adapter.EdgeFor(prior.edges.back()), last_edge.data());
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(131, 247));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedPortInsertionRemovesThePartialNodeAndRestoresSelection) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 21, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  ASSERT_NE(primary, nullptr);
  QPointer<qan::Edge> first_edge = adapter.EdgeFor(prior.edges.front());
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(163, 281));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  adapter.FailOnNext(QanOperation::InsertPort);
  const auto result = adapter.ApplyMutation(MakeAddedColorGradeMutation());

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan port insertion failed")), -1);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(adapter.EdgeFor(prior.edges.front()), first_edge.data());
  EXPECT_EQ(harness_->Graph()->getNodeCount(), 3);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(163, 281));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedEdgeInsertionRestoresRemovedEdgesAndPreservesNodeIdentity) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 22, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.a")).succeeded);
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.b")).succeeded);

  auto* develop = adapter.NodeFor(NodeId{"develop"});
  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  auto* drt     = adapter.NodeFor(NodeId{"drt"});
  auto* extra_a = adapter.NodeFor(NodeId{"grade.extra.a"});
  auto* extra_b = adapter.NodeFor(NodeId{"grade.extra.b"});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(drt, nullptr);
  ASSERT_NE(extra_a, nullptr);
  ASSERT_NE(extra_b, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(191, 313));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  auto mutation = MakeEdgeMutation(prior.edges.front(), prior.edges.back());
  adapter.FailOnNext(QanOperation::InsertEdge);
  const auto result = adapter.ApplyMutation(mutation);

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan edge creation failed")), -1);
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"}), develop);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(adapter.NodeFor(NodeId{"drt"}), drt);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra.a"}), extra_a);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra.b"}), extra_b);
  EXPECT_EQ(adapter.EdgeFor(mutation.inserted_edges.front()), nullptr);
  EXPECT_EQ(adapter.EdgeFor(mutation.inserted_edges.back()), nullptr);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(191, 313));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedEdgeBindingRemovesTheIncompleteEdgeAndRestoresTopology) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 23, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.a")).succeeded);
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.b")).succeeded);

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  ASSERT_NE(primary, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(223, 349));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  auto mutation = MakeEdgeMutation(prior.edges.front(), prior.edges.back());
  adapter.FailOnNext(QanOperation::BindEdge);
  const auto result = adapter.ApplyMutation(mutation);

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan edge binding failed")), -1);
  EXPECT_EQ(adapter.EdgeFor(mutation.inserted_edges.front()), nullptr);
  EXPECT_EQ(adapter.EdgeFor(mutation.inserted_edges.back()), nullptr);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(223, 349));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedEdgeRemovalRestoresPortListsAndPreservesVisualIdentity) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 24, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  auto* first   = adapter.EdgeFor(prior.edges.front());
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(first, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(251, 383));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  adapter.FailOnNext(QanOperation::RemoveEdge);
  const auto result = adapter.RemoveProjectedEdge(prior.edges.front());

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan edge removal failed")), -1);
  EXPECT_EQ(adapter.EdgeFor(prior.edges.front()), first);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(251, 383));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedPortRemovalRestoresTheNodeWithoutChangingOtherIdentities) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 25, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra")).succeeded);

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  auto* extra   = adapter.NodeFor(NodeId{"grade.extra"});
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(extra, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(277, 419));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  adapter.FailOnNext(QanOperation::RemovePort);
  const auto result = adapter.RemoveProjectedNode(NodeId{"grade.extra"});

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan port removal failed")), -1);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra"}), extra);
  EXPECT_NE(adapter.InputPortFor(NodeId{"grade.extra"}, kImagePort()), nullptr);
  EXPECT_NE(adapter.OutputPortFor(NodeId{"grade.extra"}, kImagePort()), nullptr);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(277, 419));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedNodeRemovalRestoresItsPortsAndKeepsTheProjectionUsable) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 26, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra")).succeeded);

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  auto* extra   = adapter.NodeFor(NodeId{"grade.extra"});
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(extra, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(299, 457));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  adapter.FailOnNext(QanOperation::RemoveNode);
  const auto result = adapter.RemoveProjectedNode(NodeId{"grade.extra"});

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan node removal failed")), -1);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra"}), extra);
  EXPECT_NE(adapter.InputPortFor(NodeId{"grade.extra"}, kImagePort()), nullptr);
  EXPECT_NE(adapter.OutputPortFor(NodeId{"grade.extra"}, kImagePort()), nullptr);
  ExpectMappedEdges(adapter, prior.edges);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(299, 457));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, VisualReversalFailureReportsBothTheOriginalAndReversalErrors) {
  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 27, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.a")).succeeded);
  ASSERT_TRUE(adapter.InsertProjectedNode(MakeExtraNode("grade.extra.b")).succeeded);

  auto* primary = adapter.NodeFor(NodeId{"grade.primary"});
  ASSERT_NE(primary, nullptr);
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(317, 491));
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  const auto mutation = MakeEdgeMutation(prior.edges.front(), prior.edges.back());
  adapter.FailOnCalls(QanOperation::InsertEdge, 2, QanOperation::RemoveEdge, 3);
  const auto result = adapter.ApplyMutation(mutation);

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("Qan edge creation failed")), -1);
  EXPECT_NE(result.error.indexOf(QStringLiteral("visual reversal failed")), -1);
  EXPECT_NE(adapter.EdgeFor(mutation.inserted_edges.front()), nullptr);
  EXPECT_EQ(adapter.EdgeFor(mutation.inserted_edges.back()), nullptr);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), primary);
  ASSERT_TRUE(adapter.NodeItemPosition(NodeId{"grade.primary"}).has_value());
  EXPECT_EQ(adapter.NodeItemPosition(NodeId{"grade.primary"}).value(), QPointF(317, 491));
  EXPECT_FALSE(adapter.DrawerOpen(NodeId{"grade.primary"}));
  ExpectSelectedNode(*harness_->Graph(), primary);
}

TEST_F(AlcedoQanGraph, FailedVisualMutationLeavesTheProductDocumentUntouched) {
  PipelineDocument document = CreateDefaultPipelineDocument();
  auto             draft    = EditorNodeGraphDraft::FromDocument(document, {});
  const auto       mutation = draft.AddColorGrade(NodeId{"grade.extra"});
  ASSERT_TRUE(mutation.succeeded);
  ASSERT_NE(draft.FindNode(NodeId{"grade.extra"}), nullptr);
  ASSERT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);

  FailureInjectingQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto prior = EditorNodeGraphProjection::Build(document, 28, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(prior).succeeded) << "initial apply";
  adapter.FailOnNext(QanOperation::InsertNode);
  const auto result = adapter.ApplyMutation(mutation);

  EXPECT_FALSE(result.succeeded);
  EXPECT_EQ(document.Graph().FindNode(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(document.NextColorGradeNameNumber(), 2u);
  EXPECT_NE(draft.FindNode(NodeId{"grade.extra"}), nullptr);
  draft.RestoreLastMutation();
  EXPECT_EQ(draft.Nodes(), prior.nodes);
  EXPECT_EQ(draft.Edges(), prior.edges);
}

TEST_F(AlcedoQanGraph, GraphDestructionClearsIdentityMaps) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded) << "initial apply";
  QPointer<qan::Graph> old_graph = harness_->Graph();
  QPointer<qan::Node>  old_node  = adapter.NodeFor(NodeId{"develop"});

  ASSERT_TRUE(harness_->Loader()->setProperty("active", false));
  ASSERT_TRUE(WaitFor([&]() { return old_graph.isNull() && harness_->Graph() == nullptr; }));
  EXPECT_TRUE(old_node.isNull());
  EXPECT_EQ(adapter.graph(), nullptr);
  EXPECT_FALSE(adapter.has_projection());
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"}), nullptr);
  EXPECT_EQ(adapter.LiveNodeId(old_node.data()), std::nullopt);
}

TEST_F(AlcedoQanGraph, EnablesRequestOnlyConnectorWithThemeColorsAndRoleConnectable) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  const auto snapshot = EditorNodeGraphProjection::Build(document, 4, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded) << "initial apply";
  adapter.ApplyProductSelection(NodeId{"grade.primary"});

  auto* graph = harness_->Graph();
  ASSERT_NE(graph, nullptr);
  ASSERT_TRUE(WaitFor([&] { return graph->getConnector() != nullptr; }));
  EXPECT_TRUE(graph->getConnectorEnabled());
  EXPECT_FALSE(graph->getConnectorCreateDefaultEdge());
  EXPECT_EQ(graph->getConnectorEdgeColor(),
            alcedo::ui::AppTheme::Instance().graphCandidateEdgeColor());
  EXPECT_EQ(graph->getConnectorColor(), alcedo::ui::AppTheme::Instance().graphPortBorderColor());
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"})->getItem()->getConnectable(),
            qan::NodeItem::Connectable::OutConnectable);
  EXPECT_EQ(adapter.NodeFor(NodeId{"drt"})->getItem()->getConnectable(),
            qan::NodeItem::Connectable::InConnectable);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"})->getItem()->getConnectable(),
            qan::NodeItem::Connectable::Connectable);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.second"})->getItem()->getConnectable(),
            qan::NodeItem::Connectable::Connectable);
  ASSERT_NE(graph->getConnector()->getSourcePort(), nullptr);
  EXPECT_EQ(graph->getConnector()->getSourcePort(),
            adapter.OutputPortFor(NodeId{"grade.primary"}, kImagePort()));
}

TEST_F(AlcedoQanGraph, ConnectorDropResolvesLiveIdsWithoutInsertingAPermanentEdge) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.second"}).empty());
  const auto snapshot = EditorNodeGraphProjection::Build(document, 5, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded) << "initial apply";
  adapter.ApplyProductSelection(NodeId{"grade.primary"});
  auto* graph = harness_->Graph();
  ASSERT_TRUE(WaitFor([&] { return graph->getConnector() != nullptr; }));
  const auto edges_before = graph->get_edge_count();

  QSignalSpy moved(&adapter, &ui::AlcedoQanGraph::ConnectorMoveRequested);
  QSignalSpy rejected(&adapter, &ui::AlcedoQanGraph::ConnectorRequestRejected);
  auto*      dest_item = adapter.NodeFor(NodeId{"drt"})->getItem();
  ASSERT_NE(dest_item, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(graph->getConnector(), "connectorReleased",
                                        Qt::DirectConnection, Q_ARG(QQuickItem*, dest_item)));
  ASSERT_EQ(moved.count(), 1);
  EXPECT_EQ(rejected.count(), 0);
  const auto args = moved.takeFirst();
  EXPECT_EQ(args.at(0).toString(), QStringLiteral("grade.primary"));
  EXPECT_EQ(args.at(1).toString(), QStringLiteral("drt"));
  EXPECT_FALSE(args.at(2).toBool());
  EXPECT_EQ(graph->get_edge_count(), edges_before);
}

TEST_F(AlcedoQanGraph, ConnectorDropOnUnknownPrimitiveRejectsWithoutCreatingAnEdge) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 6, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded) << "initial apply";
  adapter.ApplyProductSelection(NodeId{"grade.primary"});
  auto* graph = harness_->Graph();
  ASSERT_TRUE(WaitFor([&] { return graph->getConnector() != nullptr; }));
  auto* stray = graph->insertNode();
  ASSERT_NE(stray, nullptr);
  ASSERT_NE(stray->getItem(), nullptr);
  const auto edges_before = graph->get_edge_count();

  QSignalSpy rejected(&adapter, &ui::AlcedoQanGraph::ConnectorRequestRejected);
  QSignalSpy moved(&adapter, &ui::AlcedoQanGraph::ConnectorMoveRequested);
  ASSERT_TRUE(QMetaObject::invokeMethod(graph->getConnector(), "connectorReleased",
                                        Qt::DirectConnection,
                                        Q_ARG(QQuickItem*, stray->getItem())));
  ASSERT_EQ(rejected.count(), 1);
  EXPECT_EQ(moved.count(), 0);
  EXPECT_EQ(rejected.takeFirst().at(0).toString(),
            QStringLiteral("That graph item is no longer part of the current Version"));
  EXPECT_EQ(graph->get_edge_count(), edges_before);
}

TEST_F(AlcedoQanGraph, DrawerFoldKeepsPortIdentityForReconnect) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 7, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded) << "initial apply";
  auto* output = adapter.OutputPortFor(NodeId{"grade.primary"}, kImagePort());
  auto* input  = adapter.InputPortFor(NodeId{"grade.primary"}, kImagePort());
  ASSERT_NE(output, nullptr);
  ASSERT_NE(input, nullptr);
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  ASSERT_TRUE(WaitFor([&] {
    auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
    return item != nullptr && !item->property("drawerOpen").toBool();
  }));
  EXPECT_EQ(adapter.OutputPortFor(NodeId{"grade.primary"}, kImagePort()), output);
  EXPECT_EQ(adapter.InputPortFor(NodeId{"grade.primary"}, kImagePort()), input);
  EXPECT_EQ(output->getId(), QStringLiteral("out:image"));
  EXPECT_EQ(input->getId(), QStringLiteral("in:image"));
}

TEST_F(AlcedoQanGraph, IncrementalInsertAndRemovePreserveUnaffectedIdentities) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 8, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);
  const auto replace_count = adapter.topology_replace_count();
  auto*      develop       = adapter.NodeFor(NodeId{"develop"});
  auto*      primary       = adapter.NodeFor(NodeId{"grade.primary"});
  auto*      drt           = adapter.NodeFor(NodeId{"drt"});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(drt, nullptr);

  EditorNodeProjection extra;
  extra.node_id      = NodeId{"grade.extra"};
  extra.node_kind    = EditorNodeKind::ColorGrade;
  extra.display_name = "Color Grade 2";
  ASSERT_TRUE(adapter.InsertProjectedNode(extra).succeeded);
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"}), develop);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"}), primary);
  EXPECT_EQ(adapter.NodeFor(NodeId{"drt"}), drt);
  EXPECT_NE(adapter.NodeFor(NodeId{"grade.extra"}), nullptr);
  EXPECT_NE(adapter.InputPortFor(NodeId{"grade.extra"}, PortId{"image"}), nullptr);
  EXPECT_NE(adapter.OutputPortFor(NodeId{"grade.extra"}, PortId{"image"}), nullptr);

  EditorNodeEdgeProjection edge{NodeId{"develop"}, PortId{"image"}, NodeId{"grade.extra"},
                                PortId{"image"}};
  ASSERT_TRUE(adapter
                  .RemoveProjectedEdge(EditorNodeEdgeProjection{
                      NodeId{"develop"}, PortId{"image"}, NodeId{"grade.primary"}, PortId{"image"}})
                  .succeeded);
  EXPECT_NE(adapter.OutputPortFor(NodeId{"develop"}, PortId{"image"}), nullptr);
  EXPECT_NE(adapter.InputPortFor(NodeId{"grade.extra"}, PortId{"image"}), nullptr);
  const auto edge_result = adapter.InsertProjectedEdge(edge, true);
  ASSERT_TRUE(edge_result.succeeded) << edge_result.error.toStdString();
  EXPECT_EQ(adapter.topology_replace_count(), replace_count);
  ASSERT_TRUE(adapter.RemoveProjectedNode(NodeId{"grade.extra"}).succeeded);
  EXPECT_EQ(adapter.NodeFor(NodeId{"grade.extra"}), nullptr);
  EXPECT_EQ(adapter.NodeFor(NodeId{"develop"}), develop);
}

TEST_F(AlcedoQanGraph, ApplySnapshotFailsWhenColorGradeDelegateUrlIsEmpty) {
  ui::AlcedoQanGraph adapter;
  adapter.set_graph(harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 1, 1, 1);
  const auto result   = adapter.ApplySnapshot(snapshot);

  EXPECT_FALSE(result.succeeded);
  EXPECT_NE(result.error.indexOf(QStringLiteral("URL is empty")), -1);
  EXPECT_EQ(harness_->Graph()->getNodeCount(), 0);
  EXPECT_FALSE(adapter.has_projection());
}

TEST_F(AlcedoQanGraph, KeyboardConnectPinsTheSourceWhileSelectionMoves) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  const auto snapshot = EditorNodeGraphProjection::Build(CreateDefaultPipelineDocument(), 7, 2, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);
  adapter.ApplyProductSelection(NodeId{"grade.primary"});
  auto* graph = harness_->Graph();
  ASSERT_TRUE(WaitFor([&] { return graph->getConnector() != nullptr; }));

  EXPECT_TRUE(adapter.beginKeyboardConnect(QStringLiteral("grade.primary")));
  EXPECT_TRUE(adapter.keyboard_connect_active());
  EXPECT_EQ(adapter.keyboard_connect_source_id_string(), QStringLiteral("grade.primary"));
  ASSERT_NE(graph->getConnector()->getSourcePort(), nullptr);
  EXPECT_EQ(graph->getConnector()->getSourcePort(),
            adapter.OutputPortFor(NodeId{"grade.primary"}, kImagePort()));

  adapter.ApplyProductSelection(NodeId{"drt"});
  EXPECT_TRUE(adapter.keyboard_connect_active());
  EXPECT_EQ(adapter.keyboard_connect_source_id_string(), QStringLiteral("grade.primary"));
  EXPECT_EQ(graph->getConnector()->getSourcePort(),
            adapter.OutputPortFor(NodeId{"grade.primary"}, kImagePort()));

  EXPECT_FALSE(adapter.beginKeyboardConnect(QStringLiteral("drt")));
  EXPECT_TRUE(adapter.keyboard_connect_active());
  EXPECT_FALSE(adapter.beginKeyboardConnect(QStringLiteral("missing.node")));
  EXPECT_TRUE(adapter.keyboard_connect_active());

  adapter.cancelKeyboardConnect();
  EXPECT_FALSE(adapter.keyboard_connect_active());
  EXPECT_TRUE(adapter.keyboard_connect_source_id_string().isEmpty());
}

}  // namespace alcedo
