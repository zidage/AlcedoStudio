//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <filesystem>
#include <functional>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitUntil(const std::function<bool()>& pred, int timeoutMs) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    ProcessEvents(20);
  }
  return pred();
}

auto Row(const QString& label, int count) -> QVariantMap {
  return QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("count"), count}};
}

constexpr char kGraphHarness[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "dateCommitGraphHarness"
    width: 268
    height: 820
    visible: true
    color: appTheme.bgCanvasColor
    property var dateModel: []
    property string selectedDate: ""
    property int folderId: 0
    property string lastClicked: ""
    property var graphItem: host.item

    Loader {
        id: host
        objectName: "dateCommitGraphLoader"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        source: graphSourceUrl
        onLoaded: {
            if (!item)
                return
            item.model = Qt.binding(function() { return root.dateModel })
            item.selectedLabel = Qt.binding(function() { return root.selectedDate })
            item.folderKey = Qt.binding(function() { return root.folderId })
            item.dayClicked.connect(function(label) { root.lastClicked = label })
        }
    }
}
)";

constexpr char kSectionHarness[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "dateFilterSectionHarness"
    width: 268
    height: 820
    visible: true
    color: appTheme.bgCanvasColor
    property var dateModel: []
    property string selectedDate: ""
    property int folderId: 0
    property string lastClicked: ""
    property var sectionItem: host.item

    Loader {
        id: host
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        source: sectionSourceUrl
        onLoaded: {
            if (!item)
                return
            item.title = "By Capture Date"
            item.model = Qt.binding(function() { return root.dateModel })
            item.selectedLabel = Qt.binding(function() { return root.selectedDate })
            item.folderKey = Qt.binding(function() { return root.folderId })
            item.dayClicked.connect(function(label) { root.lastClicked = label })
        }
    }
}
)";

struct GraphHarness {
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           graph  = nullptr;
  QStringList           warnings;

  GraphHarness() {
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& ws) {
      for (const auto& w : ws) {
        warnings << w.toString();
      }
    });
    AppTheme::Instance().setReduceMotion(true);
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(
        QStringLiteral("graphSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/DateCommitGraph.qml")));
    engine.loadData(QByteArray{kGraphHarness},
                    QUrl(QStringLiteral("file:///DateCommitGraphHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      (void)QTest::qWaitForWindowExposed(window);
      (void)WaitUntil([this] {
        graph = window->findChild<QQuickItem*>(QStringLiteral("dateCommitGraph"));
        return graph != nullptr;
      }, 2000);
      graph = window->findChild<QQuickItem*>(QStringLiteral("dateCommitGraph"));
    }
  }

  void setModel(const QVariantList& rows) {
    window->setProperty("dateModel", rows);
    ProcessEvents(40);
  }

  void setSelected(const QString& label) {
    window->setProperty("selectedDate", label);
    ProcessEvents(20);
  }

  void setFolder(int folder_id) {
    window->setProperty("folderId", folder_id);
    ProcessEvents(20);
  }

  auto inspect(const QString& label) -> QVariantMap {
    if (graph == nullptr) {
      return {};
    }
    const QString expr =
        QStringLiteral("inspectCell(\"%1\")").arg(label);
    QQmlExpression qml_expr(qmlContext(graph), graph, expr);
    (void)qml_expr.evaluate();
    ProcessEvents(10);
    return graph->property("lastCellInfo").toMap();
  }
};

struct SectionHarness {
  QQmlApplicationEngine engine;
  QQuickWindow*         window  = nullptr;
  QQuickItem*           section = nullptr;
  QStringList           warnings;

  SectionHarness() {
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& ws) {
      for (const auto& w : ws) {
        warnings << w.toString();
      }
    });
    AppTheme::Instance().setReduceMotion(true);
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(
        QStringLiteral("sectionSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/DateFilterSection.qml")));
    engine.loadData(QByteArray{kSectionHarness},
                    QUrl(QStringLiteral("file:///DateFilterSectionHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      (void)QTest::qWaitForWindowExposed(window);
      (void)WaitUntil([this] {
        section = window->findChild<QQuickItem*>(QStringLiteral("dateFilterSection"));
        return section != nullptr;
      }, 2000);
      section = window->findChild<QQuickItem*>(QStringLiteral("dateFilterSection"));
    }
  }
};

}  // namespace

TEST(DateCommitGraphQmlTest, TokensMatchAppTheme) {
  auto& theme = AppTheme::Instance();
  EXPECT_EQ(theme.dateGraphCellMinSize(), 10);
  EXPECT_EQ(theme.dateGraphCellGap(), 3);
  EXPECT_EQ(theme.dateGraphCellRadius(), 2);
  EXPECT_EQ(theme.dateGraphLevel4Color(), theme.toneSteel());
}

TEST(DateCommitGraphQmlTest, RollingWindowEndsOnLatestPhotoAndHasYearOfDays) {
  GraphHarness h;
  ASSERT_NE(h.window, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.setModel(QVariantList{Row(QStringLiteral("(unknown)"), 3), Row(QStringLiteral("2024-02-29"), 1),
                          Row(QStringLiteral("2026-05-25"), 4)});

  EXPECT_EQ(h.graph->property("latestDate").toString(), QStringLiteral("2026-05-25"));
  EXPECT_EQ(h.graph->property("selectedYearKey").toString(), QStringLiteral("rolling"));
  EXPECT_EQ(h.graph->property("startDate").toString(), QStringLiteral("2025-05-26"));
  EXPECT_EQ(h.graph->property("endDate").toString(), QStringLiteral("2026-05-25"));
  EXPECT_EQ(h.graph->property("dayCount").toInt(), 365);
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);
}

TEST(DateCommitGraphQmlTest, CalendarLeapYearExposesEveryDay) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.setModel(QVariantList{Row(QStringLiteral("2024-02-29"), 2),
                          Row(QStringLiteral("2026-05-25"), 4)});
  ASSERT_TRUE(h.graph->setProperty("selectedYearKey", QStringLiteral("2024")));
  ProcessEvents(20);

  EXPECT_EQ(h.graph->property("selectedYearKey").toString(), QStringLiteral("2024"));
  EXPECT_EQ(h.graph->property("startDate").toString(), QStringLiteral("2024-01-01"));
  EXPECT_EQ(h.graph->property("endDate").toString(), QStringLiteral("2024-12-31"));
  EXPECT_EQ(h.graph->property("dayCount").toInt(), 366);
  const auto leap = h.inspect(QStringLiteral("2024-02-29"));
  EXPECT_TRUE(leap.value(QStringLiteral("found")).toBool());
  EXPECT_EQ(leap.value(QStringLiteral("count")).toInt(), 2);
}

TEST(DateCommitGraphQmlTest, ClickingOccupiedDayEmitsIsoLabel) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  ASSERT_NE(h.window, nullptr);

  h.setModel(QVariantList{Row(QStringLiteral("2026-05-25"), 5)});
  auto* grid = h.window->findChild<QQuickItem*>(QStringLiteral("dateCommitGraphGrid"));
  ASSERT_NE(grid, nullptr);
  const auto info = h.inspect(QStringLiteral("2026-05-25"));
  ASSERT_TRUE(info.value(QStringLiteral("found")).toBool());

  QSignalSpy spy(h.graph, SIGNAL(dayClicked(QString)));
  const QPointF local(info.value(QStringLiteral("x")).toReal(),
                      info.value(QStringLiteral("y")).toReal());
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, grid->mapToScene(local).toPoint());
  ProcessEvents(40);

  EXPECT_GE(spy.count(), 1);
  if (spy.count() > 0) {
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("2026-05-25"));
  }
  EXPECT_EQ(h.window->property("lastClicked").toString(), QStringLiteral("2026-05-25"));
}

TEST(DateCommitGraphQmlTest, DateFilterShrinkDoesNotDropCachedHistogram) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.setModel(QVariantList{Row(QStringLiteral("2025-01-02"), 1),
                          Row(QStringLiteral("2026-05-25"), 4)});
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);

  h.setSelected(QStringLiteral("2026-05-25"));
  h.setModel(QVariantList{Row(QStringLiteral("2026-05-25"), 4)});
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);
  EXPECT_EQ(h.graph->property("latestDate").toString(), QStringLiteral("2026-05-25"));
}

TEST(DateCommitGraphQmlTest, StatsRefreshBeforeSelectionKeepsYearHeatmap) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.setModel(QVariantList{Row(QStringLiteral("2025-06-01"), 1),
                          Row(QStringLiteral("2026-05-25"), 4)});
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);
  EXPECT_EQ(h.graph->property("dayCount").toInt(), 365);

  // StatsEngine emits StatsChanged (shrunk buckets) before StatsFilterChanged.
  h.setModel(QVariantList{Row(QStringLiteral("2026-05-25"), 4)});
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);
  EXPECT_EQ(h.graph->property("latestDate").toString(), QStringLiteral("2026-05-25"));
  EXPECT_EQ(h.graph->property("dayCount").toInt(), 365);
  EXPECT_TRUE(h.inspect(QStringLiteral("2025-06-01")).value(QStringLiteral("found")).toBool());
  EXPECT_EQ(h.inspect(QStringLiteral("2025-06-01")).value(QStringLiteral("count")).toInt(), 1);

  h.setSelected(QStringLiteral("2026-05-25"));
  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 2);
}

TEST(DateCommitGraphQmlTest, FolderChangeRebuildsHistogramWhileDateFilterIsActive) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.setModel(QVariantList{Row(QStringLiteral("2025-01-02"), 1),
                          Row(QStringLiteral("2026-05-25"), 4)});
  h.setSelected(QStringLiteral("2026-05-25"));
  h.setModel(QVariantList{Row(QStringLiteral("2023-08-10"), 2)});
  h.setFolder(7);

  EXPECT_EQ(h.graph->property("histogramSize").toInt(), 1);
  EXPECT_EQ(h.graph->property("latestDate").toString(), QStringLiteral("2023-08-10"));
  EXPECT_EQ(h.graph->property("endDate").toString(), QStringLiteral("2023-08-10"));
}

TEST(DateCommitGraphQmlTest, OccupiedPeakDayUsesLevelFourToken) {
  GraphHarness h;
  ASSERT_NE(h.graph, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  ASSERT_NE(h.window, nullptr);

  h.setModel(QVariantList{Row(QStringLiteral("2026-05-25"), 8)});
  const auto info = h.inspect(QStringLiteral("2026-05-25"));
  ASSERT_TRUE(info.value(QStringLiteral("found")).toBool());
  EXPECT_EQ(info.value(QStringLiteral("level")).toInt(), 4);
}

TEST(DateFilterSectionQmlTest, StyleSwitchLoadsActivityGraphWithoutQueryingStatsEngine) {
  SectionHarness h;
  ASSERT_NE(h.window, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  ASSERT_NE(h.section, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();

  h.window->setProperty("dateModel",
                        QVariantList{Row(QStringLiteral("2026-05-25"), 3)});
  ProcessEvents(40);

  EXPECT_EQ(h.section->property("styleKey").toString(), QStringLiteral("calendar"));
  auto* calendar = h.window->findChild<QQuickItem*>(QStringLiteral("dateFilterCalendarLoader"));
  ASSERT_NE(calendar, nullptr);
  EXPECT_TRUE(calendar->property("active").toBool());

  ASSERT_TRUE(h.section->setProperty("styleKey", QStringLiteral("activity")));
  ASSERT_TRUE(WaitUntil(
      [this_window = h.window] {
        auto* graph = this_window->findChild<QQuickItem*>(QStringLiteral("dateCommitGraph"));
        return graph != nullptr && graph->property("dayCount").toInt() == 365;
      },
      2000));

  auto* graph = h.window->findChild<QQuickItem*>(QStringLiteral("dateCommitGraph"));
  ASSERT_NE(graph, nullptr);
  EXPECT_EQ(graph->property("dayCount").toInt(), 365);
  EXPECT_EQ(graph->property("endDate").toString(), QStringLiteral("2026-05-25"));
  EXPECT_FALSE(h.window->findChild<QQuickItem*>(QStringLiteral("dateFilterCalendarLoader"))
                   ->property("active")
                   .toBool());
}

}  // namespace alcedo::ui::test
