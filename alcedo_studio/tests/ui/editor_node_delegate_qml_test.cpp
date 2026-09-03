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
#include <QQmlError>
#include <QQmlExtensionPlugin>
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
                         "EditorNodePortDelegate.qml", "EditorNodeEdgeDelegate.qml"};
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

}  // namespace alcedo
