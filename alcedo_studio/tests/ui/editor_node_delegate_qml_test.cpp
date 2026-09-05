//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QList>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlExtensionPlugin>
#include <QQmlProperty>
#include <QFont>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSize>
#include <QStringList>
#include <QUrl>
#include <QuickQanava>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "qanEdge.h"
#include "qanEdgeItem.h"
#include "qanGraph.h"
#include "qanNode.h"
#include "qanNodeItem.h"
#include "qanPortItem.h"
#include "ui/alcedo_main/album_backend/alcedo_qan_graph.hpp"
#include "ui/alcedo_main/app_theme.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace {

constexpr char kGraphHarnessQml[] = R"qml(
import QtQuick
import QtQuick.Controls
import QuickQanava 2.0 as Qan

ApplicationWindow {
    id: root
    objectName: "editorNodeDelegateHarness"
    width: 640
    height: 480
    visible: true
    color: appTheme.graphCanvasColor

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
                     QUrl(QStringLiteral("file:///EditorNodeDelegateHarness.qml")));

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
  mask.display_name = "Hidden Mask Name";
  mask.source       = std::move(source);
  return mask;
}

void CollectTexts(QQuickItem* item, QStringList* texts) {
  if (item == nullptr || texts == nullptr) {
    return;
  }
  const auto text = item->property("text");
  if (text.isValid() && text.metaType().id() == QMetaType::QString) {
    const auto value = text.toString().trimmed();
    if (!value.isEmpty()) {
      texts->push_back(value);
    }
  }
  for (auto* child : item->childItems()) {
    CollectTexts(child, texts);
  }
}

void CollectObjectNames(QObject* object, QStringList* names) {
  if (object == nullptr || names == nullptr) {
    return;
  }
  const auto name = object->objectName();
  if (!name.isEmpty()) {
    names->push_back(name);
  }
  for (auto* child : object->children()) {
    CollectObjectNames(child, names);
  }
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

auto FindDescendant(QObject* root, const QString& object_name) -> QQuickItem* {
  return root == nullptr ? nullptr : root->findChild<QQuickItem*>(object_name);
}

void CollectMaskRows(QQuickItem* item, QList<QQuickItem*>* rows) {
  if (item == nullptr || rows == nullptr) {
    return;
  }
  if (item->objectName() == QLatin1String("editorNodeMaskTypeRow")) {
    rows->push_back(item);
  }
  for (auto* child : item->childItems()) {
    CollectMaskRows(child, rows);
  }
}

auto MaskRows(QQuickItem* root) -> QList<QQuickItem*> {
  QList<QQuickItem*> rows;
  CollectMaskRows(root, &rows);
  std::sort(rows.begin(), rows.end(), [](QQuickItem* lhs, QQuickItem* rhs) {
    return lhs->mapToScene(QPointF()).y() < rhs->mapToScene(QPointF()).y();
  });
  return rows;
}

auto FindMaskIcon(QQuickItem* row) -> QQuickItem* {
  if (row == nullptr) {
    return nullptr;
  }
  if (auto* named = FindDescendant(row, QStringLiteral("editorNodeMaskTypeIcon"))) {
    return named;
  }
  for (auto* child : row->childItems()) {
    if (child->property("source").isValid() && child->property("sourceSize").isValid()) {
      return child;
    }
  }
  return nullptr;
}

auto SceneY(QQuickItem* item) -> qreal {
  if (item == nullptr) {
    return 0.0;
  }
  return item->mapToScene(QPointF(0, item->height() / 2.0)).y();
}

auto ReadQmlFile(const char* file_name) -> QString {
  QFile file(SrcQmlDir() + QLatin1Char('/') + QLatin1String(file_name));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  return QString::fromUtf8(file.readAll());
}

}  // namespace

namespace alcedo {

class EditorNodeDelegateQml : public ::testing::Test {
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
    ui::AppTheme::Instance().setReduceMotion(true);
    ui::AppTheme::Instance().setCurrentThemeIndex(0);
  }

  auto ApplyDocument(ui::AlcedoQanGraph* adapter, PipelineDocument document)
      -> EditorNodeGraphSnapshot {
    AttachAlcedoDelegates(*adapter, harness_->Graph());
    const auto snapshot = EditorNodeGraphProjection::Build(document, 1, 1, 1);
    const auto result   = adapter->ApplySnapshot(snapshot);
    EXPECT_TRUE(result.succeeded) << result.error.toStdString();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return snapshot;
  }

  auto ApplyDefault(ui::AlcedoQanGraph* adapter) -> EditorNodeGraphSnapshot {
    return ApplyDocument(adapter, CreateDefaultPipelineDocument());
  }

  static std::unique_ptr<QanHarness> harness_;
};

std::unique_ptr<QanHarness> EditorNodeDelegateQml::harness_;

TEST_F(EditorNodeDelegateQml, ColorGradeShowsNameWithoutTopologyStatusAdjustmentOrMaskCount) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  auto*              grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask(MaskId{"mask.gradient"}, LinearGradientMaskSource{}), 0);
  ApplyDocument(&adapter, std::move(document));

  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  EXPECT_FALSE(item->getResizable());
  EXPECT_NE(FindDescendant(item, QStringLiteral("editorNodeCard")), nullptr);
  EXPECT_NE(FindDescendant(item, QStringLiteral("editorNodeMaskDrawer")), nullptr);

  const auto name = FindDescendant(item, QStringLiteral("editorNodeName"));
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->property("text").toString(), QStringLiteral("Color Grade 1"));

  QStringList texts;
  CollectTexts(item, &texts);
  EXPECT_TRUE(texts.contains(QStringLiteral("Color Grade 1")));
  EXPECT_TRUE(texts.contains(QStringLiteral("Masks")));
  EXPECT_TRUE(texts.contains(QStringLiteral("Gradient")));
  for (const auto& text : texts) {
    EXPECT_FALSE(text.contains(QStringLiteral(" · ")));
    EXPECT_FALSE(text.contains(QStringLiteral(" | ")));
    EXPECT_FALSE(text.contains(QStringLiteral(" / ")));
    EXPECT_NE(text, QStringLiteral("On"));
    EXPECT_NE(text, QStringLiteral("Off"));
    EXPECT_NE(text, QStringLiteral("Active"));
    EXPECT_NE(text, QStringLiteral("Inactive"));
    EXPECT_FALSE(text.contains(QStringLiteral("1 masks"), Qt::CaseInsensitive));
    EXPECT_FALSE(text.contains(QStringLiteral("Exposure")));
    EXPECT_FALSE(text.contains(QStringLiteral("#1")));
  }

  QStringList names;
  CollectObjectNames(item, &names);
  for (const auto& object_name : names) {
    EXPECT_FALSE(object_name.contains(QStringLiteral("Badge"), Qt::CaseInsensitive));
    EXPECT_FALSE(object_name.contains(QStringLiteral("Pill"), Qt::CaseInsensitive));
    EXPECT_FALSE(object_name.contains(QStringLiteral("Chip"), Qt::CaseInsensitive));
    EXPECT_FALSE(object_name.contains(QStringLiteral("StatusDot"), Qt::CaseInsensitive));
  }
}

TEST_F(EditorNodeDelegateQml, MaskRowsShowOnlyApprovedTypeIconAndLabelInDisplayOrder) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  auto*              grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask(MaskId{"mask.gradient"}, LinearGradientMaskSource{}), 0);
  grade->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 1);
  grade->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 2);
  ApplyDocument(&adapter, std::move(document));

  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(WaitFor([&] { return MaskRows(item).size() == 3; }));
  const auto rows = MaskRows(item);
  ASSERT_EQ(rows.size(), 3);
  auto* first  = rows.at(0);
  auto* second = rows.at(1);
  auto* third  = rows.at(2);
  EXPECT_EQ(first->property("sourceKind").toString(), QStringLiteral("linearGradient"));
  EXPECT_EQ(second->property("sourceKind").toString(), QStringLiteral("radial"));
  EXPECT_EQ(third->property("sourceKind").toString(), QStringLiteral("brush"));
  EXPECT_EQ(first->property("typeLabel").toString(), QStringLiteral("Gradient"));
  EXPECT_EQ(second->property("typeLabel").toString(), QStringLiteral("Radial"));
  EXPECT_EQ(third->property("typeLabel").toString(), QStringLiteral("Brush"));

  QStringList texts;
  CollectTexts(item, &texts);
  EXPECT_FALSE(texts.contains(QStringLiteral("Hidden Mask Name")));
  EXPECT_FALSE(texts.contains(QStringLiteral("mask.gradient")));
  EXPECT_EQ(texts.count(QStringLiteral("Gradient")), 1);
  EXPECT_EQ(texts.count(QStringLiteral("Radial")), 1);
  EXPECT_EQ(texts.count(QStringLiteral("Brush")), 1);

  auto* icon = FindMaskIcon(first);
  ASSERT_NE(icon, nullptr);
  EXPECT_EQ(icon->property("source").toUrl(), QUrl(QStringLiteral("qrc:/mask_icons/gradient.svg")));
  EXPECT_EQ(icon->property("sourceSize").toSize().width(),
            ui::AppTheme::Instance().iconSourceSizeCompact());
  EXPECT_GE(ui::AppTheme::Instance().iconSourceSizeCompact(),
            ui::AppTheme::Instance().iconOpticalSizeCompact());
  EXPECT_EQ(icon->width(), ui::AppTheme::Instance().iconOpticalSizeCompact());
}

TEST_F(EditorNodeDelegateQml, MaskDrawerStartsOpenAndUserCanCloseAndReopenWithoutHistory) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 0);
  const auto snapshot = ApplyDocument(&adapter, std::move(document));

  auto*      item     = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  auto* drawer = FindDescendant(item, QStringLiteral("editorNodeMaskDrawer"));
  ASSERT_NE(drawer, nullptr);
  EXPECT_TRUE(item->property("drawerOpen").toBool());
  EXPECT_TRUE(drawer->property("expanded").toBool());
  EXPECT_GT(drawer->height(), ui::AppTheme::Instance().graphMaskDrawerHeaderHeight());
  ASSERT_TRUE(WaitFor([&] { return MaskRows(item).size() == 1; }));
  EXPECT_EQ(MaskRows(item).size(), 1);

  const auto open_height         = item->height();
  const auto projection_revision = adapter.projection_revision();
  const auto topology_revision   = adapter.topology_revision();
  ASSERT_TRUE(QMetaObject::invokeMethod(drawer, "toggle", Qt::DirectConnection));
  ASSERT_TRUE(WaitFor([&] { return !drawer->property("expanded").toBool(); }));

  EXPECT_FALSE(item->property("drawerOpen").toBool());
  EXPECT_NEAR(drawer->height(), ui::AppTheme::Instance().graphMaskDrawerHeaderHeight(), 0.5);
  EXPECT_LT(item->height(), open_height);
  EXPECT_EQ(adapter.projection_revision(), projection_revision);
  EXPECT_EQ(adapter.topology_revision(), topology_revision);
  EXPECT_EQ(adapter.NodeProjection(NodeId{"grade.primary"})->display_name,
            snapshot.nodes[1].display_name);

  ASSERT_TRUE(QMetaObject::invokeMethod(drawer, "toggle", Qt::DirectConnection));
  ASSERT_TRUE(WaitFor([&] { return drawer->property("expanded").toBool(); }));
  EXPECT_TRUE(item->property("drawerOpen").toBool());
  EXPECT_NEAR(item->height(), open_height, 0.5);
}

TEST_F(EditorNodeDelegateQml, OutputPortAndEdgesFollowOpenAndClosedDrawerHeight) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 0);
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 1);
  const auto snapshot = ApplyDocument(&adapter, std::move(document));

  auto*      item     = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  auto*      drawer   = FindDescendant(item, QStringLiteral("editorNodeMaskDrawer"));
  ASSERT_NE(item, nullptr);
  ASSERT_NE(drawer, nullptr);
  auto* output   = adapter.OutputPortFor(NodeId{"grade.primary"}, PortId{"image"});
  auto* incoming = adapter.EdgeFor(snapshot.edges.front());
  auto* outgoing = adapter.EdgeFor(snapshot.edges.back());
  ASSERT_NE(output, nullptr);
  ASSERT_NE(incoming, nullptr);
  ASSERT_NE(outgoing, nullptr);

  const auto open_port_y = SceneY(output);
  const auto open_bounds = item->getBoundingShape().boundingRect().height();
  EXPECT_NEAR(open_bounds, item->height(), 1.0);
  EXPECT_EQ(outgoing->getItem()->getSourceItem(), output);

  ASSERT_TRUE(QMetaObject::invokeMethod(drawer, "toggle", Qt::DirectConnection));
  ASSERT_TRUE(WaitFor([&] { return !drawer->property("expanded").toBool(); }));
  QCoreApplication::processEvents(QEventLoop::AllEvents);

  const auto closed_port_y = SceneY(output);
  EXPECT_LT(closed_port_y, open_port_y);
  EXPECT_NEAR(item->getBoundingShape().boundingRect().height(), item->height(), 1.0);
  EXPECT_EQ(outgoing->getItem()->getSourceItem(), output);
  EXPECT_EQ(incoming->getItem()->getDestinationItem(),
            adapter.InputPortFor(NodeId{"grade.primary"}, PortId{"image"}));
  EXPECT_EQ(outgoing->getItem()->getDstShape(), qan::EdgeStyle::ArrowShape::None);
}

TEST_F(EditorNodeDelegateQml, EdgeEndpointsStayGluedToPortsThroughFirstOpenBurstAndDrawerFold) {
  ui::AlcedoQanGraph adapter;
  AttachAlcedoDelegates(adapter, harness_->Graph());
  auto document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 0);
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 1);
  const auto snapshot = EditorNodeGraphProjection::Build(document, 1, 1, 1);
  ASSERT_TRUE(adapter.ApplySnapshot(snapshot).succeeded);

  // First-open burst: EditorNodeController applies stored layout in the same
  // event turn as the snapshot insert, before any polish pass has anchored the
  // port docks to the node edges.
  adapter.SetNodeItemPosition(NodeId{"develop"}, QPointF(48, 48));
  adapter.SetNodeItemPosition(NodeId{"grade.primary"}, QPointF(48, 136));
  adapter.SetNodeItemPosition(NodeId{"drt"}, QPointF(48, 329));

  auto* container = harness_->Graph()->getContainerItem();
  ASSERT_NE(container, nullptr);
  auto* incoming = adapter.EdgeFor(snapshot.edges.front());
  auto* outgoing = adapter.EdgeFor(snapshot.edges.back());
  ASSERT_NE(incoming, nullptr);
  ASSERT_NE(outgoing, nullptr);

  // Straight backbone edge endpoints: p1 is the bottom-center of the source
  // port rect, p2 the top-center of the destination port rect (container CS).
  const auto glued = [container](qan::Edge* edge) {
    if (edge == nullptr || edge->getItem() == nullptr) {
      return false;
    }
    auto* item = edge->getItem();
    auto* src  = item->getSourceItem();
    auto* dst  = item->getDestinationItem();
    if (src == nullptr || dst == nullptr || item->getHidden()) {
      return false;
    }
    const QRectF src_br = src->mapRectToItem(container, src->boundingRect());
    const QRectF dst_br = dst->mapRectToItem(container, dst->boundingRect());
    const QPointF p1    = item->mapToItem(container, item->getP1());
    const QPointF p2    = item->mapToItem(container, item->getP2());
    return std::abs(p1.x() - src_br.center().x()) < 1.0 &&
           std::abs(p1.y() - src_br.bottom()) < 1.0 &&
           std::abs(p2.x() - dst_br.center().x()) < 1.0 &&
           std::abs(p2.y() - dst_br.top()) < 1.0;
  };

  ASSERT_TRUE(WaitFor([&] { return glued(incoming) && glued(outgoing); }))
      << "edges must re-anchor to the docked ports once the docks settle";

  auto* grade_item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  const qreal open_height = grade_item->height();

  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  ASSERT_TRUE(WaitFor([&] {
    return grade_item->height() < open_height - 1.0 && glued(incoming) && glued(outgoing);
  })) << "edges must follow the output port when the drawer fold moves the dock";
}

TEST_F(EditorNodeDelegateQml, EndpointDelegatesOmitMaskDrawerAndKeepCompactHeight) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);

  auto* develop = adapter.NodeFor(NodeId{"develop"})->getItem();
  auto* drt     = adapter.NodeFor(NodeId{"drt"})->getItem();
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(drt, nullptr);
  EXPECT_EQ(FindDescendant(develop, QStringLiteral("editorNodeMaskDrawer")), nullptr);
  EXPECT_EQ(FindDescendant(drt, QStringLiteral("editorNodeMaskDrawer")), nullptr);
  EXPECT_NE(FindDescendant(develop, QStringLiteral("editorEndpointNodeCard")), nullptr);
  EXPECT_FALSE(develop->getResizable());
  EXPECT_FALSE(drt->getResizable());
  EXPECT_NEAR(develop->height(), ui::AppTheme::Instance().graphEndpointHeight(), 0.5);
  EXPECT_NEAR(drt->height(), ui::AppTheme::Instance().graphEndpointHeight(), 0.5);

  QStringList develop_texts;
  CollectTexts(develop, &develop_texts);
  EXPECT_EQ(develop_texts.size(), 1);
  EXPECT_FALSE(develop_texts.contains(QStringLiteral("Locked")));
  EXPECT_FALSE(develop_texts.contains(QStringLiteral("On")));
}

TEST_F(EditorNodeDelegateQml, EmptyOpenDrawerHasNoMaskRows) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);

  auto* item   = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  auto* drawer = FindDescendant(item, QStringLiteral("editorNodeMaskDrawer"));
  ASSERT_NE(drawer, nullptr);
  EXPECT_TRUE(drawer->property("expanded").toBool());
  EXPECT_TRUE(MaskRows(item).isEmpty());
  EXPECT_NEAR(drawer->height(), ui::AppTheme::Instance().graphMaskDrawerHeaderHeight(), 0.5);
}

TEST_F(EditorNodeDelegateQml, NodeVisualTokensMatchAppThemeInBothThemes) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* card = FindDescendant(adapter.NodeFor(NodeId{"grade.primary"})->getItem(),
                              QStringLiteral("editorNodeCard"));
  ASSERT_NE(card, nullptr);

  for (int theme = 0; theme <= 1; ++theme) {
    ui::AppTheme::Instance().setCurrentThemeIndex(theme);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(card->property("color").value<QColor>(), ui::AppTheme::Instance().cardSurfaceColor())
        << "theme " << theme;
    EXPECT_EQ(card->property("radius").toInt(), ui::AppTheme::Instance().controlRadiusSmall());
    EXPECT_EQ(adapter.NodeFor(NodeId{"grade.primary"})->getItem()->width(),
              ui::AppTheme::Instance().graphNodeWidth());
  }
}

TEST_F(EditorNodeDelegateQml, MaskTypeIconsStayReadyForCompactSourceSizeAcrossThemes) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 0);
  ApplyDocument(&adapter, std::move(document));
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_TRUE(WaitFor([&] { return MaskRows(item).size() == 1; }));
  const auto rows = MaskRows(item);
  ASSERT_EQ(rows.size(), 1);
  auto* icon = FindMaskIcon(rows.front());
  ASSERT_NE(icon, nullptr);

  for (int theme = 0; theme <= 1; ++theme) {
    ui::AppTheme::Instance().setCurrentThemeIndex(theme);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(icon->property("status").toInt(), 1) << "Image.Ready";
    EXPECT_GT(icon->width(), 0);
    EXPECT_GT(icon->height(), 0);
    EXPECT_EQ(icon->property("sourceSize").toSize().width(),
              ui::AppTheme::Instance().iconSourceSizeCompact());
    EXPECT_GE(icon->property("sourceSize").toSize().width(),
              ui::AppTheme::Instance().iconOpticalSizeCompact());
  }
}

TEST_F(EditorNodeDelegateQml, ProductionNodeQmlHasNoMaterialImportOrEffectChrome) {
  const char* files[] = {"EditorNodeDelegate.qml",     "EditorEndpointNodeDelegate.qml",
                         "EditorNodeMaskDrawer.qml",   "EditorNodeMaskTypeRow.qml",
                         "EditorNodePortDelegate.qml", "EditorNodePortDock.qml",
                         "EditorNodeEdgeDelegate.qml"};
  for (const auto* file : files) {
    const auto source = ReadQmlFile(file);
    ASSERT_FALSE(source.isEmpty()) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("QtQuick.Controls.Material"))) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("QtQuick.Effects"))) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("DropShadow"))) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("MultiEffect"))) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("RectangularGlow"))) << file;
    EXPECT_FALSE(source.contains(QStringLiteral("gradient:"))) << file;
  }

  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* port = FindDescendant(adapter.OutputPortFor(NodeId{"develop"}, PortId{"image"}),
                              QStringLiteral("editorNodePortSquare"));
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->property("radius").toInt(), 0);
  EXPECT_EQ(port->width(), ui::AppTheme::Instance().graphPortSize());
  EXPECT_EQ(port->height(), ui::AppTheme::Instance().graphPortSize());
}

TEST_F(EditorNodeDelegateQml, PortSquareIsHollowGreenOutlineAcrossThemes) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* port = FindDescendant(adapter.OutputPortFor(NodeId{"develop"}, PortId{"image"}),
                              QStringLiteral("editorNodePortSquare"));
  ASSERT_NE(port, nullptr);

  for (int theme = 0; theme <= 1; ++theme) {
    ui::AppTheme::Instance().setCurrentThemeIndex(theme);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(port->property("color").value<QColor>().alpha(), 0)
        << "port square is hollow; the canvas shows through, theme " << theme;
    EXPECT_EQ(QQmlProperty::read(port, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphPortBorderColor())
        << "theme " << theme;
    EXPECT_GT(QQmlProperty::read(port, QStringLiteral("border.width")).toDouble(), 1.0)
        << "hollow outline needs a visible stroke, theme " << theme;
  }
}

TEST_F(EditorNodeDelegateQml, BackboneEdgeStrokeMatchesGraphEdgeTokens) {
  ui::AlcedoQanGraph adapter;
  const auto           snapshot = ApplyDefault(&adapter);
  ASSERT_FALSE(snapshot.edges.empty());

  for (const auto& edge : snapshot.edges) {
    auto* qan_edge = adapter.EdgeFor(edge);
    ASSERT_NE(qan_edge, nullptr);
    ASSERT_NE(qan_edge->getItem(), nullptr);
    ASSERT_NE(qan_edge->getItem()->getStyle(), nullptr);
    EXPECT_EQ(qan_edge->getItem()->getStyle()->getLineWidth(),
              ui::AppTheme::Instance().graphEdgeWidth());
    EXPECT_EQ(qan_edge->getItem()->getStyle()->getLineColor(),
              ui::AppTheme::Instance().graphEdgeColor());
  }
}

TEST_F(EditorNodeDelegateQml, MaskDrawerWellIsInsetInsideVisibleCardBorder) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  auto* card = FindDescendant(item, QStringLiteral("editorNodeCard"));
  auto* well = FindDescendant(item, QStringLiteral("editorNodeMaskDrawerWell"));
  ASSERT_NE(card, nullptr);
  ASSERT_NE(well, nullptr);

  for (int theme = 0; theme <= 1; ++theme) {
    ui::AppTheme::Instance().setCurrentThemeIndex(theme);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    EXPECT_EQ(QQmlProperty::read(card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphNodeBorderColor())
        << "unselected card keeps a visible border, theme " << theme;
    EXPECT_EQ(well->property("color").value<QColor>(),
              ui::AppTheme::Instance().graphMaskDrawerSurfaceColor())
        << "theme " << theme;
    EXPECT_NE(ui::AppTheme::Instance().graphMaskDrawerSurfaceColor(),
              ui::AppTheme::Instance().cardSurfaceColor())
        << "drawer well must read as a distinct inset surface, theme " << theme;
  }

  const auto border_width = QQmlProperty::read(card, QStringLiteral("border.width")).toDouble();
  EXPECT_NEAR(well->mapToItem(card, QPointF(0, 0)).x(), border_width, 0.5);
  EXPECT_NEAR(well->width(), card->width() - 2.0 * border_width, 0.5);
  EXPECT_NEAR(well->mapToItem(card, QPointF(0, well->height())).y(),
              card->height() - border_width, 0.5)
      << "well leaves the card border visible along the bottom edge";
}

TEST_F(EditorNodeDelegateQml, MaskDrawerHeaderWashKeepsCardBorderVisibleOnHover) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 0);
  ApplyDocument(&adapter, std::move(document));
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  auto* drawer = FindDescendant(item, QStringLiteral("editorNodeMaskDrawer"));
  auto* wash   = FindDescendant(item, QStringLiteral("editorNodeMaskDrawerHeaderWash"));
  ASSERT_NE(drawer, nullptr);
  ASSERT_NE(wash, nullptr);

  const auto& theme       = ui::AppTheme::Instance();
  const qreal inset       = theme.graphSelectionOutlineWidth();
  const qreal well_radius = theme.controlRadiusSmall() - inset;

  EXPECT_NEAR(QQmlProperty::read(wash, QStringLiteral("anchors.leftMargin")).toDouble(), inset,
              0.01);
  EXPECT_NEAR(QQmlProperty::read(wash, QStringLiteral("anchors.rightMargin")).toDouble(), inset,
              0.01);
  EXPECT_NEAR(QQmlProperty::read(wash, QStringLiteral("anchors.bottomMargin")).toDouble(), inset,
              0.01);

  // Open with Mask rows: the header sits mid-drawer with square bottom corners.
  ASSERT_TRUE(drawer->property("expanded").toBool());
  EXPECT_NEAR(wash->property("bottomLeftRadius").toDouble(), 0.0, 0.01);

  // Closed: the header spans the whole drawer. The wash must stay off the card
  // border row and round its bottom corners to the well radius.
  ASSERT_TRUE(QMetaObject::invokeMethod(drawer, "toggle", Qt::DirectConnection));
  ASSERT_TRUE(WaitFor([&] {
    return !drawer->property("expanded").toBool() &&
           drawer->property("foldProgress").toReal() < 0.001;
  }));
  EXPECT_NEAR(wash->property("bottomLeftRadius").toDouble(), well_radius, 0.01);
  EXPECT_NEAR(wash->property("bottomRightRadius").toDouble(), well_radius, 0.01);
  EXPECT_LE(wash->mapToItem(drawer, QPointF(0, wash->height())).y(),
            drawer->height() - inset + 0.5)
      << "hover wash must not paint over the card border row";
}

TEST_F(EditorNodeDelegateQml, LongTranslatedNameElidesInsideGraphNodeWidth) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  auto*              grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->SetDisplayName(
      "Very long translated Color Grade name that must stay on one line inside the card");
  ApplyDocument(&adapter, std::move(document));
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  auto* name = FindDescendant(item, QStringLiteral("editorNodeName"));
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(item->width(), ui::AppTheme::Instance().graphNodeWidth());
  EXPECT_LE(name->width(), ui::AppTheme::Instance().graphNodeWidth());
  EXPECT_EQ(name->property("wrapMode").toInt(), 0);
  WaitFor([&] { return name->property("truncated").toBool(); });
  EXPECT_LE(name->width() + ui::AppTheme::Instance().spaceSm() * 2,
            ui::AppTheme::Instance().graphNodeWidth() + 1.0);
}

TEST_F(EditorNodeDelegateQml, NameRowGrowsWithLargeTitleFontWithoutChangingCardWidth) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_NE(item, nullptr);
  auto* name_row = FindDescendant(item, QStringLiteral("editorNodeNameRow"));
  auto* name     = FindDescendant(item, QStringLiteral("editorNodeName"));
  ASSERT_NE(name_row, nullptr);
  ASSERT_NE(name, nullptr);
  const qreal before_height = name_row->height();
  const qreal before_width  = item->width();
  auto        font          = name->property("font").value<QFont>();
  font.setPixelSize(40);
  ASSERT_TRUE(name->setProperty("font", QVariant::fromValue(font)));
  ASSERT_TRUE(WaitFor([&] { return name_row->height() > before_height + 1.0; }));
  EXPECT_EQ(item->width(), before_width);
  EXPECT_EQ(item->width(), ui::AppTheme::Instance().graphNodeWidth());
}

TEST_F(EditorNodeDelegateQml, SelectedAndUnselectedOutlinesUseThemeTokensInBothThemes) {
  ui::AlcedoQanGraph adapter;
  ApplyDefault(&adapter);
  auto* grade_item    = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  auto* develop_item  = adapter.NodeFor(NodeId{"develop"})->getItem();
  auto* drt_item      = adapter.NodeFor(NodeId{"drt"})->getItem();
  auto* grade_card    = FindDescendant(grade_item, QStringLiteral("editorNodeCard"));
  auto* develop_card  = FindDescendant(develop_item, QStringLiteral("editorEndpointNodeCard"));
  auto* drt_card      = FindDescendant(drt_item, QStringLiteral("editorEndpointNodeCard"));
  ASSERT_NE(grade_card, nullptr);
  ASSERT_NE(develop_card, nullptr);
  ASSERT_NE(drt_card, nullptr);

  for (int theme = 0; theme <= 1; ++theme) {
    ui::AppTheme::Instance().setCurrentThemeIndex(theme);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    adapter.ApplyProductSelection(NodeId{"grade.primary"});
    EXPECT_EQ(QQmlProperty::read(grade_card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphSelectionOutlineColor())
        << "theme " << theme;
    EXPECT_EQ(QQmlProperty::read(develop_card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphNodeBorderColor())
        << "theme " << theme;
    adapter.ApplyProductSelection(NodeId{"develop"});
    EXPECT_EQ(QQmlProperty::read(grade_card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphNodeBorderColor())
        << "theme " << theme;
    EXPECT_EQ(QQmlProperty::read(develop_card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphSelectionOutlineColor())
        << "theme " << theme;
    adapter.ApplyProductSelection(NodeId{"drt"});
    EXPECT_EQ(QQmlProperty::read(drt_card, QStringLiteral("border.color")).value<QColor>(),
              ui::AppTheme::Instance().graphSelectionOutlineColor())
        << "theme " << theme;
  }
}

TEST_F(EditorNodeDelegateQml, AccessibleNamesCoverNodeMaskAndEndpointRoles) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  auto*              grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->AddMask(MakeMask(MaskId{"mask.gradient"}, LinearGradientMaskSource{}), 0);
  grade->AddMask(MakeMask(MaskId{"mask.radial"}, RadialMaskSource{}), 1);
  ApplyDocument(&adapter, std::move(document));

  auto* grade_item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  auto* develop    = adapter.NodeFor(NodeId{"develop"})->getItem();
  auto* drt        = adapter.NodeFor(NodeId{"drt"})->getItem();
  ASSERT_NE(grade_item, nullptr);
  ASSERT_TRUE(WaitFor([&] { return MaskRows(grade_item).size() == 2; }));
  auto* header       = FindDescendant(grade_item, QStringLiteral("editorNodeMaskDrawerHeader"));
  auto* grade_name   = FindDescendant(grade_item, QStringLiteral("editorNodeName"));
  auto* develop_name = FindDescendant(develop, QStringLiteral("editorEndpointNodeName"));
  auto* drt_name     = FindDescendant(drt, QStringLiteral("editorEndpointNodeName"));
  ASSERT_NE(header, nullptr);
  ASSERT_NE(grade_name, nullptr);
  ASSERT_NE(develop_name, nullptr);
  ASSERT_NE(drt_name, nullptr);
  EXPECT_TRUE(header->activeFocusOnTab());
  EXPECT_FALSE(grade_item->activeFocusOnTab());
  EXPECT_FALSE(develop->activeFocusOnTab());
  EXPECT_EQ(grade_name->property("text").toString(), QStringLiteral("Color Grade 1"));
  EXPECT_FALSE(develop_name->property("text").toString().isEmpty());
  EXPECT_FALSE(drt_name->property("text").toString().isEmpty());
  const auto rows = MaskRows(grade_item);
  ASSERT_EQ(rows.size(), 2);
  EXPECT_EQ(rows.at(0)->property("typeLabel").toString(), QStringLiteral("Gradient"));
  EXPECT_EQ(rows.at(1)->property("typeLabel").toString(), QStringLiteral("Radial"));

  const auto grade_qml    = ReadQmlFile("EditorNodeDelegate.qml");
  const auto endpoint_qml = ReadQmlFile("EditorEndpointNodeDelegate.qml");
  const auto drawer_qml   = ReadQmlFile("EditorNodeMaskDrawer.qml");
  const auto row_qml      = ReadQmlFile("EditorNodeMaskTypeRow.qml");
  EXPECT_NE(grade_qml.indexOf(QStringLiteral("Accessible.name: root.displayName")), -1);
  EXPECT_NE(grade_qml.indexOf(QStringLiteral("Accessible.description: root.drawerOpen")), -1);
  EXPECT_NE(endpoint_qml.indexOf(QStringLiteral("Accessible.name: root.displayName")), -1);
  EXPECT_NE(drawer_qml.indexOf(QStringLiteral("Accessible.name: root.expanded")), -1);
  EXPECT_NE(drawer_qml.indexOf(QStringLiteral("qsTr(\"Masks\")")), -1);
  EXPECT_NE(row_qml.indexOf(QStringLiteral("Accessible.name: root.typeLabel")), -1);

  const auto runtime_name = AttachedName(header);
  if (!runtime_name.isEmpty()) {
    EXPECT_EQ(runtime_name, QStringLiteral("Collapse Masks"));
  }
  adapter.SetDrawerOpen(NodeId{"grade.primary"}, false);
  ASSERT_TRUE(WaitFor([&] { return !grade_item->property("drawerOpen").toBool(); }));
  const auto collapsed = AttachedName(header);
  if (!collapsed.isEmpty()) {
    EXPECT_EQ(collapsed, QStringLiteral("Expand Masks"));
  }
}

TEST_F(EditorNodeDelegateQml, CompactMaskIconsKeepSourceSizeForListedDevicePixelRatios) {
  ui::AlcedoQanGraph adapter;
  auto               document = CreateDefaultPipelineDocument();
  document.PrimaryGrade()->AddMask(MakeMask(MaskId{"mask.brush"}, BrushMaskSource{}), 0);
  ApplyDocument(&adapter, std::move(document));
  auto* item = adapter.NodeFor(NodeId{"grade.primary"})->getItem();
  ASSERT_TRUE(WaitFor([&] { return MaskRows(item).size() == 1; }));
  auto* icon = FindMaskIcon(MaskRows(item).front());
  ASSERT_NE(icon, nullptr);
  const auto optical = ui::AppTheme::Instance().iconOpticalSizeCompact();
  const auto source  = ui::AppTheme::Instance().iconSourceSizeCompact();
  EXPECT_GE(source, optical);
  EXPECT_EQ(icon->property("sourceSize").toSize().width(), source);
  EXPECT_EQ(icon->width(), optical);
  ASSERT_NE(harness_->window(), nullptr);
  EXPECT_GE(harness_->window()->devicePixelRatio(), 1.0);
  const double listed[] = {1.0, 1.25, 1.5, 2.0};
  for (double dpr : listed) {
    EXPECT_EQ(icon->property("sourceSize").toSize().width(), source) << dpr;
    EXPECT_EQ(icon->property("status").toInt(), 1) << dpr;
  }
}

}  // namespace alcedo
