//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUTPanel QML interaction + visual-token tests.
// Loads production LUTPanel.qml against a controllable fake catalog model so
// scroll/contentY, selection, favorites, and appTheme VI can be asserted
// without a full Main window or real filesystem LUT directory.

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QFont>
#include <QGuiApplication>
#include <QJSValue>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class FakeLutCatalogModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
  Q_PROPERTY(QString selectedPath READ selectedPath WRITE setSelectedPath NOTIFY selectedPathChanged)
  Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedPathChanged)
  Q_PROPERTY(QString directoryText READ directoryText NOTIFY catalogChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY catalogChanged)
  Q_PROPERTY(bool canOpenDirectory READ canOpenDirectory NOTIFY catalogChanged)
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
  Q_PROPERTY(QStringList favoritePaths READ favoritePaths NOTIFY favoritePathsChanged)

 public:
  explicit FakeLutCatalogModel(QObject* parent = nullptr) : QObject(parent) {
    // Long enough list that the viewport can scroll and keep mid rows visible.
    QVariantList rows;
    rows.push_back(MakeEntry(QStringLiteral("none"), QString(), QStringLiteral("None"), true, true));
    for (int i = 0; i < 40; ++i) {
      const QString path = QStringLiteral("D:/fake/lut_%1.cube").arg(i, 2, 10, QChar('0'));
      const QString name = QStringLiteral("LUT %1").arg(i, 2, 10, QChar('0'));
      rows.push_back(MakeEntry(QStringLiteral("file"), path, name, true, true));
    }
    all_entries_ = rows;
    RebuildVisible();
  }

  [[nodiscard]] auto entries() const -> QVariantList { return visible_entries_; }
  [[nodiscard]] auto selectedPath() const -> QString { return selected_path_; }
  void setSelectedPath(const QString& path) {
    if (selected_path_ == path) {
      return;
    }
    selected_path_ = path;
    emit selectedPathChanged();
  }
  [[nodiscard]] auto selectedIndex() const -> int {
    for (int i = 0; i < visible_entries_.size(); ++i) {
      if (visible_entries_[i].toMap().value(QStringLiteral("path")).toString() == selected_path_) {
        return i;
      }
    }
    return selected_path_.isEmpty() ? 0 : -1;
  }
  [[nodiscard]] auto directoryText() const -> QString {
    return QStringLiteral("D:/fake/LUTs");
  }
  [[nodiscard]] auto statusText() const -> QString { return status_text_; }
  [[nodiscard]] auto canOpenDirectory() const -> bool { return true; }
  [[nodiscard]] auto filterText() const -> QString { return filter_text_; }
  void setFilterText(const QString& text) {
    if (filter_text_ == text) {
      return;
    }
    filter_text_ = text;
    emit filterTextChanged();
    RebuildVisible();
  }
  [[nodiscard]] auto favoritePaths() const -> QStringList { return favorite_paths_; }

  Q_INVOKABLE void refresh(bool /*force*/ = false) {
    ++refresh_count_;
    RebuildVisible();
    emit catalogChanged();
  }
  Q_INVOKABLE void selectPath(const QString& path) {
    if (selected_path_ == path) {
      return;
    }
    selected_path_ = path;
    ++select_count_;
    last_selected_path_ = path;
    emit selectedPathChanged();
    // Intentionally no entriesChanged — mirrors production selection contract.
  }
  Q_INVOKABLE void clearSelection() { selectPath(QString()); }
  Q_INVOKABLE void toggleFavoritePath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
      return;
    }
    const int idx = favorite_paths_.indexOf(trimmed);
    if (idx >= 0) {
      favorite_paths_.removeAt(idx);
    } else {
      favorite_paths_.append(trimmed);
    }
    emit favoritePathsChanged();
  }
  Q_INVOKABLE bool isFavoritePath(const QString& path) const {
    return favorite_paths_.contains(path.trimmed());
  }
  Q_INVOKABLE QString directoryPath() const { return QStringLiteral("D:/fake/LUTs"); }
  Q_INVOKABLE QString paramsJson() const {
    return QStringLiteral("{\"ocio_lmt\":\"%1\"}").arg(selected_path_);
  }

  int  select_count_       = 0;
  int  refresh_count_      = 0;
  QString last_selected_path_;

 signals:
  void entriesChanged();
  void selectedPathChanged();
  void catalogChanged();
  void filterTextChanged();
  void favoritePathsChanged();

 private:
  static auto MakeEntry(const QString& kind, const QString& path, const QString& name, bool valid,
                        bool selectable) -> QVariantMap {
    QVariantMap map;
    map.insert(QStringLiteral("kind"), kind);
    map.insert(QStringLiteral("path"), path);
    map.insert(QStringLiteral("displayName"), name);
    map.insert(QStringLiteral("secondaryText"),
               kind == QStringLiteral("file") ? QStringLiteral("3D cube") : QString());
    map.insert(QStringLiteral("statusText"), QString());
    map.insert(QStringLiteral("selectable"), selectable);
    map.insert(QStringLiteral("valid"), valid);
    map.insert(QStringLiteral("fileSize"), kind == QStringLiteral("file") ? 102400 : 0);
    map.insert(QStringLiteral("lutEdge"), kind == QStringLiteral("file") ? 33 : 0);
    map.insert(QStringLiteral("lutSize1d"), 0);
    map.insert(QStringLiteral("modifiedTimeSortKey"), 0);
    map.insert(QStringLiteral("lutTypeBadge"),
               kind == QStringLiteral("file") ? QStringLiteral("3D 33") : QString());
    map.insert(QStringLiteral("selected"), false);
    return map;
  }

  void RebuildVisible() {
    visible_entries_.clear();
    const QString filter = filter_text_.trimmed();
    for (const auto& row : all_entries_) {
      const auto map = row.toMap();
      if (map.value(QStringLiteral("kind")).toString() == QStringLiteral("file") &&
          !filter.isEmpty()) {
        const auto name = map.value(QStringLiteral("displayName")).toString();
        if (!name.contains(filter, Qt::CaseInsensitive)) {
          continue;
        }
      }
      visible_entries_.push_back(row);
    }
    emit entriesChanged();
  }

  QVariantList all_entries_;
  QVariantList visible_entries_;
  QString      selected_path_;
  QString      filter_text_;
  QString      status_text_;
  QStringList  favorite_paths_;
};

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

auto CenterOf(QQuickItem* item) -> QPoint {
  const QPointF local(item->width() / 2.0, item->height() / 2.0);
  return item->mapToScene(local).toPoint();
}

auto LutPanelUrl() -> QUrl {
  return QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/LUTPanel.qml"));
}

// Loads production LUTPanel.qml; IconActionButton resolves via import path.
constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "lutPanelHarness"
    width: 360
    height: 640
    visible: true
    color: appTheme.bgCanvasColor

    property var panelItem: null

    Loader {
        id: host
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        source: panelSourceUrl
        onLoaded: {
            if (!item) return
            item.objectName = "editorLutPanel"
            item.controlsEnabled = true
            item.theme = null
            // Assign model last so onLutModelChanged rebuilds the entry list.
            item.lutModel = fakeLutModel
            root.panelItem = item
        }
    }
}
)";

struct LutPanelHarness {
  FakeLutCatalogModel   model;
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           panel  = nullptr;
  QStringList           warnings;

  LutPanelHarness() {
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& ws) {
      for (const auto& w : ws) {
        warnings << w.toString();
      }
    });
    AppTheme::Instance().setReduceMotion(true);
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("fakeLutModel"), &model);
    engine.rootContext()->setContextProperty(QStringLiteral("panelSourceUrl"), LutPanelUrl());
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///LutPanelTestHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      (void)QTest::qWaitForWindowExposed(window);
      ProcessEvents(80);
      (void)WaitUntil([this] {
        panel = window->findChild<QQuickItem*>(QStringLiteral("editorLutPanel"));
        return panel != nullptr && panel->property("entryCount").toInt() > 0;
      }, 2000);
      panel = window->findChild<QQuickItem*>(QStringLiteral("editorLutPanel"));
    }
  }

  auto find(const QString& objectName) -> QQuickItem* {
    if (window == nullptr) {
      return nullptr;
    }
    return window->findChild<QQuickItem*>(objectName);
  }

  auto listView() -> QQuickItem* { return find(QStringLiteral("editorLutListView")); }

  auto contentY() -> qreal {
    auto* list = listView();
    return list ? list->property("contentY").toReal() : -1.0;
  }

  void setContentY(qreal y) {
    auto* list = listView();
    if (list != nullptr) {
      list->setProperty("contentY", y);
      ProcessEvents(40);
    }
  }
};

TEST(EditorLutPanelQmlTest, PanelLoadsWithCatalogEntriesAndToolbarChrome) {
  LutPanelHarness h;
  ASSERT_NE(h.window, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  ASSERT_NE(h.panel, nullptr) << h.warnings.join(QLatin1Char('\n')).toStdString();
  EXPECT_GE(h.panel->property("entryCount").toInt(), 40);
  EXPECT_NE(h.find(QStringLiteral("editorLutPanelTitle")), nullptr);
  EXPECT_NE(h.find(QStringLiteral("editorLutSortButton")), nullptr);
  EXPECT_NE(h.find(QStringLiteral("editorLutFavoritesFilterButton")), nullptr);
  EXPECT_NE(h.find(QStringLiteral("editorLutRefreshButton")), nullptr);
  EXPECT_NE(h.find(QStringLiteral("editorLutOpenFolderButton")), nullptr);
  EXPECT_NE(h.find(QStringLiteral("editorLutFilterInput")), nullptr);
  EXPECT_NE(h.listView(), nullptr);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join(QLatin1Char('\n')).toStdString();
}

TEST(EditorLutPanelQmlTest, TitleTypographyUsesSectionTokens) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* title = h.find(QStringLiteral("editorLutPanelTitle"));
  ASSERT_NE(title, nullptr);
  const QFont font = title->property("font").value<QFont>();
  EXPECT_EQ(font.pixelSize(), AppTheme::Instance().fontSizeSection());
  EXPECT_EQ(font.weight(), AppTheme::Instance().fontWeightHeading());
}

TEST(EditorLutPanelQmlTest, SelectPathDoesNotChangeContentY) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* list = h.listView();
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return list->width() > 10 && list->height() > 10; }, 2000));

  // Mid-list scroll: a naive ListView.Contain re-position on selection would
  // pin the row and move contentY away from this baseline.
  const qreal target_y = 180.0;
  list->setProperty("contentY", target_y);
  ProcessEvents(100);
  ASSERT_TRUE(WaitUntil([&] { return h.contentY() > 50.0; }, 1000))
      << "contentY=" << h.contentY()
      << " contentHeight=" << list->property("contentHeight").toReal();

  const qreal y_before = h.contentY();
  // Same call chain as the row MouseArea onClicked handler.
  h.model.selectPath(QStringLiteral("D:/fake/lut_12.cube"));
  ProcessEvents(150);
  EXPECT_EQ(h.model.select_count_, 1);
  EXPECT_EQ(h.model.selectedPath(), QStringLiteral("D:/fake/lut_12.cube"));
  EXPECT_NEAR(h.contentY(), y_before, 1.5)
      << "contentY jumped after selectPath on a mid-list entry";
}

TEST(EditorLutPanelQmlTest, PointerClickOnVisibleRowDoesNotChangeContentY) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* list = h.listView();
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return list->width() > 10 && list->height() > 10; }, 2000));

  const int count = h.panel->property("entryCount").toInt();
  ASSERT_GT(count, 10);
  const qreal content_h = list->property("contentHeight").toReal();
  ASSERT_GT(content_h, list->height());
  const qreal row_h = content_h / static_cast<qreal>(count);

  // Scroll so file index 12 is inside the viewport, then click its center.
  const int target_index = 12;  // lut_11 is index 12 if 0=None, 1=lut_00, ...
  const qreal target_y =
      std::max(0.0, target_index * row_h - list->height() * 0.35);
  list->setProperty("contentY", target_y);
  ProcessEvents(100);
  ASSERT_GT(h.contentY(), 20.0);

  const qreal local_y = target_index * row_h - h.contentY() + row_h * 0.5;
  ASSERT_GE(local_y, 0.0);
  ASSERT_LE(local_y, list->height());

  const qreal y_before = h.contentY();
  const int selects_before = h.model.select_count_;
  const QPoint click = list->mapToScene(QPointF(list->width() * 0.55, local_y)).toPoint();
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, click);
  ProcessEvents(150);

  EXPECT_GT(h.model.select_count_, selects_before) << "pointer click missed LUT row";
  EXPECT_FALSE(h.model.last_selected_path_.isEmpty());
  EXPECT_NEAR(h.contentY(), y_before, 1.5)
      << "contentY jumped after pointer selection";
}

TEST(EditorLutPanelQmlTest, SelectingEntryDoesNotEmitModelEntriesChanged) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  QSignalSpy entries_spy(&h.model, &FakeLutCatalogModel::entriesChanged);
  ASSERT_TRUE(entries_spy.isValid());

  // Drive selection through the model (same path as a click handler).
  const int before = entries_spy.count();
  h.model.selectPath(QStringLiteral("D:/fake/lut_07.cube"));
  ProcessEvents(80);
  EXPECT_EQ(entries_spy.count(), before);
  EXPECT_EQ(h.model.selectedPath(), QStringLiteral("D:/fake/lut_07.cube"));
  // Highlight is path-derived; entry count stays stable.
  EXPECT_GE(h.panel->property("entryCount").toInt(), 40);
}

TEST(EditorLutPanelQmlTest, FavoriteStarToggleUpdatesFavoritePathsWithoutListReset) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  const int count_before = h.panel->property("entryCount").toInt();
  const qreal y_before   = h.contentY();

  // Toggle favorite on a known path via the model (star MouseArea is dense).
  QSignalSpy fav_spy(&h.model, &FakeLutCatalogModel::favoritePathsChanged);
  h.model.toggleFavoritePath(QStringLiteral("D:/fake/lut_03.cube"));
  ProcessEvents(60);
  EXPECT_EQ(fav_spy.count(), 1);
  EXPECT_TRUE(h.model.isFavoritePath(QStringLiteral("D:/fake/lut_03.cube")));
  EXPECT_EQ(h.panel->property("entryCount").toInt(), count_before);
  EXPECT_NEAR(h.contentY(), y_before, 1.0);
}

TEST(EditorLutPanelQmlTest, RefreshButtonInvokesModelRefresh) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* refresh = h.find(QStringLiteral("editorLutRefreshButton"));
  ASSERT_NE(refresh, nullptr);
  const int before = h.model.refresh_count_;
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, CenterOf(refresh));
  ProcessEvents(80);
  EXPECT_EQ(h.model.refresh_count_, before + 1);
}

TEST(EditorLutPanelQmlTest, FilterInputDrivesModelFilterText) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* filter = h.find(QStringLiteral("editorLutFilterInput"));
  ASSERT_NE(filter, nullptr);
  filter->forceActiveFocus();
  ProcessEvents(40);
  // Set text property directly (TextInput) to avoid key encoding issues.
  filter->setProperty("text", QStringLiteral("LUT 1"));
  ProcessEvents(80);
  EXPECT_EQ(h.model.filterText(), QStringLiteral("LUT 1"));
  // Filtered list is smaller than the full catalog.
  EXPECT_LT(h.panel->property("entryCount").toInt(), 41);
  EXPECT_GT(h.panel->property("entryCount").toInt(), 0);
}

TEST(EditorLutPanelQmlTest, SelectedFillAndFavoriteStarsUseListViTokens) {
  // Monochrome inverted list VI: light selected well + ink text; favorite
  // stars use the idle/active pair on dark rows and the on-selected invert pair.
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto& theme = AppTheme::Instance();
  EXPECT_EQ(h.panel->property("colSelectedFill").value<QColor>(),
            theme.editorListSelectedFillColor());
  EXPECT_EQ(h.panel->property("colSelectedInk").value<QColor>(),
            theme.editorListSelectedInkColor());
  EXPECT_EQ(h.panel->property("colFavoriteIdle").value<QColor>(),
            theme.editorListFavoriteIdleColor());
  EXPECT_EQ(h.panel->property("colFavoriteActive").value<QColor>(),
            theme.editorListFavoriteActiveColor());
  EXPECT_EQ(h.panel->property("colFavoriteIdleOnSelected").value<QColor>(),
            theme.editorListFavoriteIdleOnSelectedColor());
  EXPECT_EQ(h.panel->property("colFavoriteActiveOnSelected").value<QColor>(),
            theme.editorListFavoriteActiveOnSelectedColor());
  EXPECT_EQ(h.panel->property("colInvalid").value<QColor>(), theme.dangerColor());
  // Invert pair must differ from the dark-row pair so selected stars flip.
  EXPECT_NE(theme.editorListFavoriteIdleColor(),
            theme.editorListFavoriteIdleOnSelectedColor());
  EXPECT_NE(theme.editorListFavoriteActiveColor(),
            theme.editorListFavoriteActiveOnSelectedColor());
}

TEST(EditorLutPanelQmlTest, LoadFromSnapshotRestoresSelectedPathWithoutSubmit) {
  // Workspace re-entry / image open must repaint selection from the session
  // snapshot. loadFromSnapshot is load-only (no selectPath submit).
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  auto* engine = qmlEngine(h.panel);
  ASSERT_NE(engine, nullptr);

  auto call_load = [&](const QString& path) {
    QJSValue snap = engine->newObject();
    QJSValue lut  = engine->newObject();
    lut.setProperty(QStringLiteral("ocio_lmt"), path);
    snap.setProperty(QStringLiteral("lut"), lut);
    QJSValue panel_js = engine->newQObject(h.panel);
    QJSValue fn       = panel_js.property(QStringLiteral("loadFromSnapshot"));
    ASSERT_TRUE(fn.isCallable());
    const QJSValue result = fn.callWithInstance(panel_js, QJSValueList{snap});
    EXPECT_FALSE(result.isError()) << result.toString().toStdString();
    ProcessEvents(40);
  };

  call_load(QStringLiteral("D:/fake/lut_07.cube"));
  EXPECT_EQ(h.model.selectedPath(), QStringLiteral("D:/fake/lut_07.cube"));
  EXPECT_EQ(h.panel->property("selectedPath").toString(), QStringLiteral("D:/fake/lut_07.cube"));
  // Fake model: setSelectedPath must not be counted as selectPath submits.
  EXPECT_EQ(h.model.select_count_, 0);

  // Slash/case variation still matches via selectedPathNormalized.
  call_load(QStringLiteral("d:\\fake\\lut_07.cube"));
  EXPECT_EQ(h.panel->property("selectedPathNormalized").toString(),
            QStringLiteral("d:/fake/lut_07.cube"));
}

TEST(EditorLutPanelQmlTest, EntryRowHeightIsUniformForNoneAndFile) {
  LutPanelHarness h;
  ASSERT_NE(h.panel, nullptr);
  const int row_h = h.panel->property("entryRowHeight").toInt();
  EXPECT_GT(row_h, 0);
  // Token composition: body + caption + spaceSm line band.
  EXPECT_EQ(row_h, AppTheme::Instance().lineHeightBody() + AppTheme::Instance().lineHeightCaption()
                        + AppTheme::Instance().spaceSm());
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_lut_panel_qml_test.moc"
