//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQmlExtensionPlugin>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QuickQanava>
#include <functional>
#include <mutex>

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace {

constexpr char kGraphHarnessQml[]  = R"qml(
import QtQuick
import QtQuick.Controls
import QuickQanava 2.0 as Qan

ApplicationWindow {
    id: root
    objectName: "quickQanavaHarness"
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
            property var graphNode: graphTopology.insertedNode

            graph: Qan.Graph {
                id: graphTopology
                objectName: "qanGraph"
                property var insertedNode: null

                Component.onCompleted: {
                    var node = graphTopology.insertNode()
                    if (!node) {
                        console.error("QuickQanava graph node creation failed")
                        return
                    }
                    node.label = "Read-only proof node"
                    graphTopology.insertedNode = node
                }
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

    property var graphViewObject: graphLoader.item
    property var graphObject: graphViewObject ? graphViewObject.graph : null
    property var graphNodeObject: graphViewObject ? graphViewObject.graphNode : null
    property int graphNodeCount: graphObject ? graphObject.getNodeCount() : 0
    property string graphNodeLabel: graphNodeObject ? graphNodeObject.label : ""
}
)qml";

constexpr char kMissingModuleQml[] = R"qml(
import QtQuick
import QuickQanavaUnavailable 1.0

Item {}
)qml";

void           SetBasicStyle() {
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

QObject* ObjectProperty(QObject* object, const char* property_name) {
  if (object == nullptr) {
    return nullptr;
  }
  return object->property(property_name).value<QObject*>();
}

class QuickQanavaHarness final {
 public:
  QuickQanavaHarness() {
    QObject::connect(&engine_, &QQmlApplicationEngine::warnings, &engine_,
                     [this](const QList<QQmlError>& errors) {
                       for (const auto& error : errors) {
                         warnings_.append(error.toString());
                       }
                     });

    SetBasicStyle();
    engine_.addImportPath(QStringLiteral("qrc:/"));
    QuickQanava::initialize(&engine_);
    engine_.loadData(QByteArray{kGraphHarnessQml},
                     QUrl(QStringLiteral("file:///QuickQanavaHarness.qml")));

    if (!engine_.rootObjects().isEmpty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().constFirst());
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  }

  QQuickWindow*      window() const { return window_; }
  const QStringList& warnings() const { return warnings_; }

 private:
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

std::string WarningText(const QStringList& warnings) {
  return warnings.join(QLatin1Char('\n')).toStdString();
}

}  // namespace

TEST(QuickQanavaProductionIntegrationTest, BasicStyleLoadsOneReadOnlyGraphNode) {
  QuickQanavaHarness harness;

  ASSERT_NE(harness.window(), nullptr) << WarningText(harness.warnings());
  ASSERT_TRUE(harness.warnings().isEmpty()) << WarningText(harness.warnings());
  EXPECT_EQ(QQuickStyle::name(), QStringLiteral("Basic"));

  QObject* graph_view = harness.window()->findChild<QObject*>("qanGraphView");
  QObject* graph      = harness.window()->findChild<QObject*>("qanGraph");
  QObject* node       = ObjectProperty(harness.window(), "graphNodeObject");

  ASSERT_NE(graph_view, nullptr);
  ASSERT_NE(graph, nullptr);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(ObjectProperty(graph_view, "graph"), graph);
  EXPECT_EQ(harness.window()->property("graphObject").value<QObject*>(), graph);
  EXPECT_EQ(harness.window()->property("graphNodeCount").toInt(), 1);
  EXPECT_EQ(harness.window()->property("graphNodeLabel").toString(),
            QStringLiteral("Read-only proof node"));
  EXPECT_NE(ObjectProperty(node, "item"), nullptr);
}

TEST(QuickQanavaProductionIntegrationTest, LoaderRecreatesGraphViewAndTopologyWithoutStaleObjects) {
  QuickQanavaHarness harness;

  ASSERT_NE(harness.window(), nullptr) << WarningText(harness.warnings());
  ASSERT_TRUE(harness.warnings().isEmpty()) << WarningText(harness.warnings());

  QObject* loader = harness.window()->findChild<QObject*>("graphLoader");
  ASSERT_NE(loader, nullptr);

  constexpr int kRebuildCount = 5;
  for (int rebuild = 0; rebuild < kRebuildCount; ++rebuild) {
    QPointer<QObject> old_graph_view{harness.window()->findChild<QObject*>("qanGraphView")};
    QPointer<QObject> old_graph{harness.window()->findChild<QObject*>("qanGraph")};
    ASSERT_FALSE(old_graph_view.isNull());
    ASSERT_FALSE(old_graph.isNull());

    ASSERT_TRUE(loader->setProperty("active", false));
    const auto graph_cleared = [&harness]() {
      return harness.window()->findChild<QObject*>("qanGraphView") == nullptr &&
             harness.window()->findChild<QObject*>("qanGraph") == nullptr &&
             ObjectProperty(harness.window(), "graphViewObject") == nullptr &&
             ObjectProperty(harness.window(), "graphObject") == nullptr &&
             ObjectProperty(harness.window(), "graphNodeObject") == nullptr;
    };
    ASSERT_TRUE(WaitFor(graph_cleared));
    EXPECT_TRUE(old_graph_view.isNull());
    EXPECT_TRUE(old_graph.isNull());
    EXPECT_EQ(harness.window()->property("graphNodeCount").toInt(), 0);
    EXPECT_TRUE(harness.window()->property("graphNodeLabel").toString().isEmpty());

    ASSERT_TRUE(loader->setProperty("active", true));
    ASSERT_TRUE(WaitFor([&harness]() {
      return harness.window()->findChild<QObject*>("qanGraphView") != nullptr &&
             harness.window()->findChild<QObject*>("qanGraph") != nullptr &&
             ObjectProperty(harness.window(), "graphNodeObject") != nullptr &&
             harness.window()->property("graphNodeCount").toInt() == 1;
    }));
    EXPECT_EQ(harness.window()->property("graphNodeLabel").toString(),
              QStringLiteral("Read-only proof node"));
  }

  EXPECT_TRUE(harness.warnings().isEmpty()) << WarningText(harness.warnings());
}

TEST(QuickQanavaProductionIntegrationTest,
     MissingQuickQanavaImportReturnsQmlErrorWithoutRootObject) {
  SetBasicStyle();
  QQmlApplicationEngine engine;
  QStringList           warnings;
  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                   [&warnings](const QList<QQmlError>& errors) {
                     for (const auto& error : errors) {
                       warnings.append(error.toString());
                     }
                   });

  engine.loadData(QByteArray{kMissingModuleQml},
                  QUrl(QStringLiteral("file:///MissingQuickQanavaImport.qml")));

  EXPECT_TRUE(engine.rootObjects().isEmpty());
  ASSERT_FALSE(warnings.isEmpty());
  const QString error_text = warnings.join(QLatin1Char('\n'));
  EXPECT_NE(error_text.indexOf(QStringLiteral("QuickQanavaUnavailable")), -1);
  EXPECT_NE(error_text.indexOf(QStringLiteral("not installed")), -1);
}
