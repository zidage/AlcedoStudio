//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/qan_delegate_library.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QUrl>
#include <QuickQanava>
#include <filesystem>
#include <memory>
#include <mutex>

#include "qanGraph.h"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace {

constexpr char kHarnessQml[] = R"qml(
import QtQuick
import QtQuick.Controls
import QuickQanava 2.0 as Qan

ApplicationWindow {
    width: 320
    height: 240
    visible: true

    Qan.GraphView {
        anchors.fill: parent
        navigable: false

        graph: Qan.Graph {
            objectName: "delegateGraph"
        }
    }
}
)qml";

auto           QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto QmlFileUrl(const char* file_name) -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QLatin1Char('/') + QLatin1String(file_name));
}

void SetBasicStyle() {
  static std::once_flag style_once;
  std::call_once(style_once, [] { QQuickStyle::setStyle(QStringLiteral("Basic")); });
}

class QanHarness final {
 public:
  QanHarness() {
    SetBasicStyle();
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    QuickQanava::initialize(&engine_);
    engine_.loadData(QByteArray{kHarnessQml},
                     QUrl(QStringLiteral("file:///QanDelegateHarness.qml")));
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    if (!engine_.rootObjects().isEmpty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().constFirst());
    }
  }

  [[nodiscard]] auto graph() const -> qan::Graph* {
    return window_ == nullptr ? nullptr : window_->findChild<qan::Graph*>("delegateGraph");
  }

 private:
  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
};

void Configure(alcedo::ui::QanDelegateLibrary& library) {
  library.Configure(QmlFileUrl("EditorNodeDelegate.qml"),
                    QmlFileUrl("EditorEndpointNodeDelegate.qml"),
                    QmlFileUrl("EditorNodePortDelegate.qml"), QmlFileUrl("EditorNodePortDock.qml"),
                    QmlFileUrl("EditorNodeEdgeDelegate.qml"));
}

}  // namespace

namespace alcedo::ui {

TEST(QanDelegateLibraryTest, LoadsAllDelegatesAndInstallsGraphOwnedComponentsWithoutController) {
  QanHarness harness;
  ASSERT_NE(harness.graph(), nullptr);
  auto* engine = qmlEngine(harness.graph());
  ASSERT_NE(engine, nullptr);

  QanDelegateLibrary library;
  Configure(library);
  EXPECT_TRUE(library.EnsureLoaded(*engine, *harness.graph()).isEmpty());
  EXPECT_NE(library.ComponentFor(EditorNodeKind::ColorGrade), nullptr);
  EXPECT_NE(library.ComponentFor(EditorNodeKind::Develop), nullptr);
  EXPECT_NE(library.EdgeComponent(), nullptr);
  EXPECT_NE(harness.graph()->property("portDelegate").value<QQmlComponent*>(), nullptr);
  EXPECT_NE(harness.graph()->property("horizontalDockDelegate").value<QQmlComponent*>(), nullptr);
  EXPECT_NE(harness.graph()->property("selectionDelegate").value<QQmlComponent*>(), nullptr);
}

TEST(QanDelegateLibraryTest, ResetDropsEngineCacheAndReloadsForARecreatedEngine) {
  QanHarness first;
  QanHarness second;
  ASSERT_NE(first.graph(), nullptr);
  ASSERT_NE(second.graph(), nullptr);
  auto* first_engine  = qmlEngine(first.graph());
  auto* second_engine = qmlEngine(second.graph());
  ASSERT_NE(first_engine, nullptr);
  ASSERT_NE(second_engine, nullptr);
  ASSERT_NE(first_engine, second_engine);

  QanDelegateLibrary library;
  Configure(library);
  ASSERT_TRUE(library.EnsureLoaded(*first_engine, *first.graph()).isEmpty());
  auto* first_component = library.ComponentFor(EditorNodeKind::ColorGrade);
  ASSERT_NE(first_component, nullptr);

  library.Reset();
  EXPECT_EQ(library.ComponentFor(EditorNodeKind::ColorGrade), nullptr);
  EXPECT_EQ(library.EdgeComponent(), nullptr);

  EXPECT_TRUE(library.EnsureLoaded(*second_engine, *second.graph()).isEmpty());
  EXPECT_NE(library.ComponentFor(EditorNodeKind::ColorGrade), nullptr);
  EXPECT_NE(library.ComponentFor(EditorNodeKind::ColorGrade), first_component);
  EXPECT_NE(second.graph()->property("portDelegate").value<QQmlComponent*>(), nullptr);
  EXPECT_NE(second.graph()->property("selectionDelegate").value<QQmlComponent*>(), nullptr);
}

}  // namespace alcedo::ui
