//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file collections_panel_qml_test.cpp
/// @brief Collections sidebar tree and image-menu "Add to" sub-menu (issue #732).
///
/// Loads the production CollectionsPanel.qml and ImageContextMenu.qml from the source tree
/// against a C++ fake folder controller. The fake keeps the production PascalCase signal names
/// and reassigns ui folder ids on every tree change, like FolderController::ReloadTree.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QMimeData>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>

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

auto WaitUntil(const std::function<bool()>& pred, int timeoutMs = 2000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    ProcessEvents(20);
  }
  return pred();
}

/// Folder tree keyed by element id (root = element 0). Rebuild() publishes rows in the
/// production shape and gives every folder a new ui id, as a tree reload does.
class FakeFolderController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList folders READ Folders NOTIFY FoldersChanged)
  Q_PROPERTY(uint currentFolderId READ CurrentFolderId NOTIFY FolderSelectionChanged)

 public:
  struct Node {
    uint    element_        = 0;
    uint    parent_element_ = 0;
    QString name_;
    /// The mount fields of FolderController (Mounted Folder Plan, MF6): 0 for an ordinary row.
    uint    mount_id_      = 0;
    bool    mount_root_    = false;
    bool    mount_offline_ = false;
    uint    mount_missing_ = 0;
  };

  void SetTree(std::vector<Node> nodes, uint current_element) {
    nodes_           = std::move(nodes);
    current_element_ = current_element;
    Rebuild();
  }

  [[nodiscard]] auto Folders() const -> QVariantList { return rows_; }
  [[nodiscard]] auto CurrentFolderId() const -> uint { return UiOf(current_element_); }
  [[nodiscard]] auto UiOf(uint element) const -> uint {
    const auto it = ui_by_element_.find(element);
    return it == ui_by_element_.end() ? 0u : it->second;
  }
  [[nodiscard]] auto ElementOf(uint ui) const -> uint {
    for (const auto& [element, ui_id] : ui_by_element_) {
      if (ui_id == ui) {
        return element;
      }
    }
    return 0u;
  }
  [[nodiscard]] auto ParentOf(uint element) const -> uint {
    for (const auto& node : nodes_) {
      if (node.element_ == element) {
        return node.parent_element_;
      }
    }
    return 0u;
  }

  QStringList calls;

  Q_INVOKABLE void SelectFolder(uint folderUiId) {
    current_element_ = ElementOf(folderUiId);
    calls << QStringLiteral("SelectFolder:%1").arg(current_element_);
    emit FolderSelectionChanged();
    emit folderSelectionChanged();
  }

  Q_INVOKABLE uint CreateFolderIn(uint parentFolderUiId, const QString& name) {
    const uint parent = ElementOf(parentFolderUiId);
    calls << QStringLiteral("CreateFolderIn:%1:%2").arg(parent).arg(name);
    const uint created = next_element_++;
    nodes_.push_back({created, parent, name});
    Rebuild();
    return created;
  }

  Q_INVOKABLE bool RenameFolder(uint folderUiId, const QString& name) {
    const uint element = ElementOf(folderUiId);
    calls << QStringLiteral("RenameFolder:%1:%2").arg(element).arg(name);
    for (auto& node : nodes_) {
      if (node.element_ == element) {
        node.name_ = name;
      }
    }
    Rebuild();
    return true;
  }

  Q_INVOKABLE bool MoveFolder(uint folderUiId, uint newParentFolderUiId) {
    const uint element = ElementOf(folderUiId);
    const uint parent  = ElementOf(newParentFolderUiId);
    calls << QStringLiteral("MoveFolder:%1:%2").arg(element).arg(parent);
    for (auto& node : nodes_) {
      if (node.element_ == element) {
        node.parent_element_ = parent;
      }
    }
    Rebuild();
    return true;
  }

  Q_INVOKABLE void DeleteFolder(uint folderUiId) {
    calls << QStringLiteral("DeleteFolder:%1").arg(ElementOf(folderUiId));
  }

  /// Adds a mount root named after the directory below the parent row, as a finished mount
  /// creation does.
  Q_INVOKABLE void MountFolder(uint parentFolderUiId, const QUrl& directory) {
    const uint    parent = ElementOf(parentFolderUiId);
    const QString path   = directory.toLocalFile();
    calls << QStringLiteral("MountFolder:%1:%2").arg(parent).arg(path);
    const uint created = next_element_++;
    nodes_.push_back({created, parent, QFileInfo(path).fileName(), next_mount_++, true, false, 0});
    Rebuild();
  }

  Q_INVOKABLE void SynchronizeMount(uint folderUiId) {
    calls << QStringLiteral("SynchronizeMount:%1").arg(ElementOf(folderUiId));
  }
  Q_INVOKABLE void Unmount(uint folderUiId) {
    calls << QStringLiteral("Unmount:%1").arg(ElementOf(folderUiId));
  }
  Q_INVOKABLE void ShowMountedFolderInFileManager(uint folderUiId) {
    calls << QStringLiteral("ShowMountedFolderInFileManager:%1").arg(ElementOf(folderUiId));
  }
  Q_INVOKABLE void RemoveMissingPhotos(uint folderUiId) {
    calls << QStringLiteral("RemoveMissingPhotos:%1").arg(ElementOf(folderUiId));
  }

  /// Publishes photo counts by element id (root = element 0), as a finished count read does:
  /// the rows are republished with the same ui ids. A row without a count reads -1.
  void SetImageCounts(std::map<uint, qint64> counts) {
    counts_ = std::move(counts);
    Publish();
    emit FoldersChanged();
  }

 signals:
  void FoldersChanged();
  void FolderSelectionChanged();
  void folderSelectionChanged();

 private:
  void Rebuild() {
    ++generation_;
    ui_by_element_.clear();
    ui_by_element_[0] = 0;
    uint index        = 1;
    for (const auto& node : nodes_) {
      ui_by_element_[node.element_] = generation_ * 1000 + index++;
    }
    Publish();
    emit FoldersChanged();
    emit FolderSelectionChanged();
    emit folderSelectionChanged();
  }

  void Publish() {
    const auto count_of = [this](uint element) -> qint64 {
      const auto it = counts_.find(element);
      return it == counts_.end() ? -1 : it->second;
    };
    QVariantList rows;
    rows.push_back(QVariantMap{{"folderId", 0u},
                               {"elementId", 0u},
                               {"parentFolderId", 0u},
                               {"parentElementId", 0u},
                               {"name", QStringLiteral("Root")},
                               {"imageCount", count_of(0)},
                               {"mountId", 0u},
                               {"structureEditable", true}});
    for (const auto& node : nodes_) {
      rows.push_back(QVariantMap{{"folderId", UiOf(node.element_)},
                                 {"elementId", node.element_},
                                 {"parentFolderId", UiOf(node.parent_element_)},
                                 {"parentElementId", node.parent_element_},
                                 {"name", node.name_},
                                 {"imageCount", count_of(node.element_)},
                                 {"mountId", node.mount_id_},
                                 {"mountRoot", node.mount_root_},
                                 {"mountOffline", node.mount_offline_},
                                 {"mountMissingCount", node.mount_missing_},
                                 {"structureEditable", node.mount_id_ == 0}});
    }
    rows_ = rows;
  }

  std::vector<Node>      nodes_;
  std::map<uint, uint>   ui_by_element_;
  std::map<uint, qint64> counts_;
  QVariantList           rows_;
  uint                 current_element_ = 0;
  uint                 generation_      = 0;
  uint                 next_element_    = 500;
  uint                 next_mount_      = 100;
};

constexpr char kHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    objectName: "collectionsHarness"
    width: 640
    height: 720
    visible: true
    color: appTheme.bgCanvasColor

    // The Main.qml surface that the panel reads through `theme` and `host`.
    property real panelRadius: 12
    property color colBgPanel: "#202022"
    property color colText: "#f0f0f0"
    property color colHover: "#444444"
    property color colAccentPrimary: "#4c8bf5"
    property string headlineFontFamily: appTheme.headlineFontFamily
    property color colButtonSecondary: "#3a3f44"
    property color colOverlay: Qt.rgba(0, 0, 0, 0.5)
    property color colDanger: "#d9534f"
    property Item dialogBlurSource: null

    // Main.qml: a disabled secondary action is the translucent idle fill.
    function secondaryButtonFill(enabled, hovered, pressed) {
        if (!enabled)
            return Qt.rgba(colButtonSecondary.r, colButtonSecondary.g, colButtonSecondary.b, 0.55)
        return hovered ? Qt.lighter(colButtonSecondary, 1.34) : colButtonSecondary
    }
    function filledActionFill(enabled, hovered, pressed) {
        return enabled ? appTheme.editorListSelectedFillColor : appTheme.bgBaseColor
    }
    function filledActionInk(enabled) {
        return enabled ? appTheme.editorListSelectedInkColor : appTheme.textMutedColor
    }

    property var panelItem: panelLoader.item
    property var menuItem: null
    property string lastMenuSignal: ""
    property int lastCollectionId: -1
    property var lastDropUrls: []
    property int lastDropFolderId: -1
    property string lastImportSignal: ""

    // The Qt Quick folder dialog draws inside this window; the test opens no system dialog.
    function useQuickDialog(dialog) {
        dialog.options = FolderDialog.DontUseNativeDialog
    }

    Loader {
        id: panelLoader
        x: 0
        y: 0
        width: 276
        height: root.height
        source: panelSourceUrl
        onLoaded: {
            item.folderController = folderFake
            item.theme = root
            item.host = root
            item.backendInteractive = true
            item.filesDropped.connect(function(urls, folderId) {
                root.lastDropUrls = urls
                root.lastDropFolderId = folderId
            })
            item.importRequested.connect(function() { root.lastImportSignal = "images" })
            item.importFromFolderRequested.connect(function() { root.lastImportSignal = "folder" })
        }
    }

    function createMenu() {
        const component = Qt.createComponent(menuSourceUrl)
        if (component.status !== Component.Ready) {
            console.warn(component.errorString())
            return
        }
        root.menuItem = component.createObject(root.contentItem, {
            collectionRows: Qt.binding(function() { return folderFake.folders }),
            currentCollectionId: Qt.binding(function() { return folderFake.currentFolderId }),
            addToEnabled: true
        })
        root.menuItem.exportQueueRequested.connect(function() { root.lastMenuSignal = "export" })
        root.menuItem.collectionRequested.connect(function(folderId) {
            root.lastMenuSignal = "collection"
            root.lastCollectionId = folderId
        })
    }
}
)";

auto FindItem(QQuickItem* parent, const QString& object_name) -> QQuickItem* {
  if (parent == nullptr) {
    return nullptr;
  }
  if (parent->objectName() == object_name) {
    return parent;
  }
  for (QQuickItem* child : parent->childItems()) {
    if (auto* found = FindItem(child, object_name)) {
      return found;
    }
  }
  return nullptr;
}

auto SceneCenter(QQuickItem* item) -> QPoint {
  return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
}

struct Harness {
  FakeFolderController  fake;
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           panel  = nullptr;
  QStringList           warnings;

  explicit Harness(std::vector<FakeFolderController::Node> nodes, uint current_element = 0) {
    fake.SetTree(std::move(nodes), current_element);
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& ws) {
      for (const auto& w : ws) {
        warnings << w.toString();
      }
    });
    AppTheme::Instance().setReduceMotion(true);
    // The production style (main.cpp); Qt accepts it only before the first Controls import.
    static const bool kStyleSet = [] {
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      return true;
    }();
    (void)kStyleSet;
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("folderFake"), &fake);
    engine.rootContext()->setContextProperty(
        QStringLiteral("panelSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/CollectionsPanel.qml")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("menuSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/ImageContextMenu.qml")));
    engine.loadData(QByteArray{kHarness}, QUrl(QStringLiteral("file:///CollectionsHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      window->show();
      window->requestActivate();
      (void)QTest::qWaitForWindowExposed(window);
      (void)WaitUntil([this] {
        panel = qvariant_cast<QQuickItem*>(window->property("panelItem"));
        return panel != nullptr;
      });
      ProcessEvents(50);
    }
  }

  auto Row(uint element) -> QQuickItem* {
    return FindItem(window->contentItem(), QStringLiteral("collectionRow_%1").arg(element));
  }
  /// A collapsed branch keeps its rows in the model with zero height.
  auto RowShown(uint element) -> bool {
    auto* row = Row(element);
    return row != nullptr && row->isVisible() && row->height() > 0.0;
  }
  auto Tree() -> QQuickItem* { return FindItem(window->contentItem(), QStringLiteral("collectionsTree")); }
  void Invoke(const char* method, const QVariant& arg) {
    ASSERT_TRUE(QMetaObject::invokeMethod(panel, method, Q_ARG(QVariant, arg)));
    ProcessEvents(30);
  }
  void SetExpanded(uint element, bool expanded) {
    ASSERT_TRUE(QMetaObject::invokeMethod(panel, "setExpanded", Q_ARG(QVariant, element),
                                          Q_ARG(QVariant, expanded)));
    ProcessEvents(30);
  }
  void FocusTree() {
    auto* tree = Tree();
    ASSERT_NE(tree, nullptr);
    tree->forceActiveFocus();
    ProcessEvents(20);
    ASSERT_TRUE(tree->hasActiveFocus());
  }
  void Key(Qt::Key key) {
    QTest::keyClick(window, key);
    ProcessEvents(40);
  }
  void Drag(QQuickItem* from, QQuickItem* to) {
    const QPoint start = SceneCenter(from);
    const QPoint end   = SceneCenter(to);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 8; ++step) {
      QTest::mouseMove(window, start + (end - start) * step / 8);
      ProcessEvents(10);
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    ProcessEvents(60);
  }
  /// Drag @p urls from the system onto the center of @p onto and drop them there. Returns
  /// whether the row showed the drop-target style before the drop.
  /// The file manager offers Copy and Move with Shift held, so it proposes Move.
  auto DropUrls(QQuickItem* onto, const QList<QUrl>& urls) -> bool {
    constexpr Qt::DropActions kOffered = Qt::CopyAction | Qt::MoveAction;
    QMimeData                 mime;
    mime.setUrls(urls);
    const QPoint    pos = SceneCenter(onto);
    QDragEnterEvent enter(pos, kOffered, &mime, Qt::NoButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(pos, kOffered, &mime, Qt::NoButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &move);
    ProcessEvents(30);
    const bool highlighted = onto->property("dropTarget").toBool();
    QDropEvent drop(QPointF(pos), kOffered, &mime, Qt::NoButton, Qt::ShiftModifier);
    EXPECT_EQ(drop.proposedAction(), Qt::MoveAction);
    QCoreApplication::sendEvent(window, &drop);
    last_drop_action = drop.isAccepted() ? drop.dropAction() : Qt::IgnoreAction;
    ProcessEvents(30);
    return highlighted;
  }
  Qt::DropAction last_drop_action = Qt::IgnoreAction;
  [[nodiscard]] auto LastDropFolderId() const -> int {
    return window->property("lastDropFolderId").toInt();
  }
  void ClearLastDrop() {
    window->setProperty("lastDropFolderId", -1);
    window->setProperty("lastDropUrls", QVariantList{});
  }
};

// Root, A (1) > A1 (2) > A1x (3), and B (4).
auto SmallTree() -> std::vector<FakeFolderController::Node> {
  return {{1, 0, QStringLiteral("A")},
          {2, 1, QStringLiteral("A1")},
          {3, 2, QStringLiteral("A1x")},
          {4, 0, QStringLiteral("B")}};
}

// Root, Trips (1), the mount root Archive (2) with the mounted folder Day1 (3), and the offline
// mount root Card (4).
auto MountTree(bool archive_offline = false) -> std::vector<FakeFolderController::Node> {
  return {{1, 0, QStringLiteral("Trips")},
          {2, 0, QStringLiteral("Archive"), 1, true, archive_offline, 3},
          {3, 2, QStringLiteral("Day1"), 1, false, archive_offline, 0},
          {4, 0, QStringLiteral("Card"), 2, true, true, 0}};
}

/// A tree taller than the panel: a mount root with a collapsed child at the top, then 60
/// ordinary folders F00..F59 (elements 10..69).
auto LongTree() -> std::vector<FakeFolderController::Node> {
  std::vector<FakeFolderController::Node> nodes{
      {5, 0, QStringLiteral("A Mount"), 1, true, false, 0},
      {6, 5, QStringLiteral("Day1"), 1, false, false, 0}};
  for (uint i = 0; i < 60; ++i) {
    nodes.push_back({10 + i, 0, QStringLiteral("F%1").arg(i, 2, 10, QLatin1Char('0'))});
  }
  return nodes;
}

/// The menu item @p object_name of the open menu @p menu.
auto MenuItem(QObject* menu, const QString& object_name) -> QObject* {
  return menu != nullptr ? menu->findChild<QObject*>(object_name) : nullptr;
}

}  // namespace

TEST(CollectionsPanelQmlTest, TokensMatchTheIssueValues) {
  auto& theme = AppTheme::Instance();
  EXPECT_EQ(theme.collectionRowHeight(), 26);
  EXPECT_EQ(theme.collectionIndentStep(), 14);
  EXPECT_EQ(theme.collectionMenuMaxRows(), 12);
  EXPECT_EQ(theme.collectionMenuFilterThreshold(), 15);
}

// In Chinese mode the UI family stays Inter (Chinese glyphs come from the Noto Sans SC
// substitution). With Noto Sans SC as the primary family, Latin capitals sat 0.07 em below the
// center of the line box (lower case 0.16 em), so tree row names looked low.
TEST(CollectionsPanelQmlTest, ChineseModeKeepsLatinTextVerticallyCentered) {
  AppTheme::SetEffectiveLanguageCode(QStringLiteral("zh_CN"));
  const QString family = AppTheme::Instance().uiFontFamily();
  AppTheme::SetEffectiveLanguageCode(QStringLiteral("en"));
  EXPECT_EQ(family, QStringLiteral("Inter"));

  QFont font(family);
  font.setPixelSize(13);
  const QFontMetricsF metrics(font);
  const qreal box_center = (metrics.ascent() - metrics.descent()) / 2.0;
  const qreal cap_center = metrics.capHeight() / 2.0;
  EXPECT_LT(std::abs(box_center - cap_center), 0.03 * 13.0)
      << "box center " << box_center << " cap center " << cap_center;
}

TEST(CollectionsPanelQmlTest, TreeStartsCollapsedWithCompactRowsAndExpandsOnRequest) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();

  ASSERT_TRUE(h.RowShown(1));
  EXPECT_TRUE(h.RowShown(4));
  EXPECT_FALSE(h.RowShown(2));
  EXPECT_FALSE(h.RowShown(3));
  EXPECT_DOUBLE_EQ(h.Row(1)->height(), AppTheme::Instance().collectionRowHeight());

  h.SetExpanded(1, true);
  EXPECT_TRUE(h.RowShown(2));
  EXPECT_FALSE(h.RowShown(3));

  // Sibling order follows the names; children follow their parent.
  EXPECT_LT(h.Row(1)->y(), h.Row(2)->y());
  EXPECT_LT(h.Row(2)->y(), h.Row(4)->y());
  // No QML warning, such as a handler that reaches no signal of the controller.
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

TEST(CollectionsPanelQmlTest, CurrentFolderIsRevealedAndExpansionSurvivesATreeReload) {
  Harness h(SmallTree(), /*current_element=*/3);
  ASSERT_NE(h.panel, nullptr);
  EXPECT_TRUE(h.RowShown(2));
  EXPECT_TRUE(h.RowShown(3));

  // A rename reloads the tree and reassigns every ui id; the expansion is keyed by element id.
  ASSERT_TRUE(h.fake.RenameFolder(h.fake.UiOf(4), QStringLiteral("Bee")));
  ProcessEvents(50);
  EXPECT_TRUE(h.RowShown(2));
  EXPECT_TRUE(h.RowShown(3));
  EXPECT_TRUE(h.RowShown(4));
}

TEST(CollectionsPanelQmlTest, KeyboardMovesSelectionExpandsCollapsesAndOpensEditors) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);
  h.FocusTree();

  h.Key(Qt::Key_Down);
  EXPECT_EQ(h.fake.ElementOf(h.fake.CurrentFolderId()), 1u);
  h.Key(Qt::Key_Right);
  EXPECT_TRUE(h.RowShown(2));
  h.Key(Qt::Key_Right);
  EXPECT_EQ(h.fake.ElementOf(h.fake.CurrentFolderId()), 2u);
  h.Key(Qt::Key_Left);
  EXPECT_EQ(h.fake.ElementOf(h.fake.CurrentFolderId()), 1u);
  h.Key(Qt::Key_Left);
  EXPECT_FALSE(h.RowShown(2));

  h.Key(Qt::Key_F2);
  ASSERT_TRUE(WaitUntil([&] {
    return FindItem(h.window->contentItem(), QStringLiteral("collectionRenameField")) != nullptr;
  }));
  h.Key(Qt::Key_Escape);
  EXPECT_TRUE(WaitUntil([&] {
    return FindItem(h.window->contentItem(), QStringLiteral("collectionRenameField")) == nullptr;
  }));
  EXPECT_FALSE(h.fake.calls.join(',').contains(QStringLiteral("RenameFolder")));

  h.FocusTree();
  h.Key(Qt::Key_Delete);
  auto* dialog = h.panel->findChild<QObject*>(QStringLiteral("collectionDeleteDialog"));
  ASSERT_NE(dialog, nullptr);
  EXPECT_TRUE(WaitUntil([&] { return dialog->property("opened").toBool(); }));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "confirmed"));
  ProcessEvents(30);
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("DeleteFolder:1")))
      << h.fake.calls.join(',').toStdString();
}

// Issue #733: every row shows the photo count of its collection; the root row shows the
// project count.
TEST(CollectionsPanelQmlTest, RowsShowThePhotoCountInCaptionInk) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr);
  auto* root_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_0"));
  auto* a_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_1"));
  auto* b_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_4"));
  ASSERT_NE(root_count, nullptr);
  ASSERT_NE(a_count, nullptr);
  ASSERT_NE(b_count, nullptr);
  // No count read yet: no label.
  EXPECT_FALSE(a_count->isVisible());

  h.fake.SetImageCounts({{0, 1234}, {1, 3}, {2, 2}, {3, 1}, {4, 0}});
  ASSERT_TRUE(WaitUntil([&] { return a_count->isVisible(); }));
  EXPECT_EQ(root_count->property("text").toString(), QLocale().toString(1234.0, 'f', 0));
  EXPECT_EQ(a_count->property("text").toString(), QStringLiteral("3"));
  EXPECT_TRUE(b_count->isVisible());
  EXPECT_EQ(b_count->property("text").toString(), QStringLiteral("0"));

  const auto& theme = AppTheme::Instance();
  EXPECT_EQ(a_count->property("color").value<QColor>(), theme.textMutedColor());
  EXPECT_EQ(b_count->property("color").value<QColor>(), theme.textColor());
  EXPECT_EQ(a_count->property("font").value<QFont>().pixelSize(), theme.fontSizeCaption());
  // The count sits right of the name, inside the row.
  // The row layout places a label that turns visible on its next polish.
  auto* a_row = h.Row(1);
  ASSERT_NE(a_row, nullptr);
  EXPECT_TRUE(WaitUntil([&] {
    return a_count->mapToItem(a_row, QPointF(0, 0)).x() > a_row->width() / 2.0;
  })) << "count x " << a_count->mapToItem(a_row, QPointF(0, 0)).x() << " row width "
      << a_row->width();
  const QPointF count_right = a_count->mapToItem(a_row, QPointF(a_count->width(), 0));
  EXPECT_LE(count_right.x(), a_row->width());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// A count update republishes the rows with the same ui ids. It changes the labels only: an
// open rename editor stays open and commits nothing.
TEST(CollectionsPanelQmlTest, CountUpdateKeepsTheRenameEditorOpen) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr);
  h.fake.SetImageCounts({{0, 5}, {1, 2}, {4, 3}});
  auto* a_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_1"));
  ASSERT_NE(a_count, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return a_count->property("text").toString() == "2"; }));

  h.FocusTree();
  h.Key(Qt::Key_F2);
  QQuickItem* field = nullptr;
  ASSERT_TRUE(WaitUntil([&] {
    field = FindItem(h.window->contentItem(), QStringLiteral("collectionRenameField"));
    return field != nullptr && field->hasActiveFocus();
  }));
  // The renamed row hides its count while the editor is open.
  auto* b_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_4"));
  ASSERT_NE(b_count, nullptr);
  EXPECT_FALSE(b_count->isVisible());

  h.fake.SetImageCounts({{0, 6}, {1, 3}, {4, 3}});
  ASSERT_TRUE(WaitUntil([&] { return a_count->property("text").toString() == "3"; }));
  ProcessEvents(50);
  ASSERT_EQ(FindItem(h.window->contentItem(), QStringLiteral("collectionRenameField")), field);
  EXPECT_TRUE(field->hasActiveFocus());
  EXPECT_FALSE(h.fake.calls.join(',').contains(QStringLiteral("RenameFolder")))
      << h.fake.calls.join(',').toStdString();
}

TEST(CollectionsPanelQmlTest, RenameFieldCommitsTheNewName) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr);
  h.FocusTree();
  h.Key(Qt::Key_F2);
  QQuickItem* field = nullptr;
  ASSERT_TRUE(WaitUntil([&] {
    field = FindItem(h.window->contentItem(), QStringLiteral("collectionRenameField"));
    return field != nullptr && field->hasActiveFocus();
  }));
  field->setProperty("text", QStringLiteral("Beach"));
  h.Key(Qt::Key_Return);
  EXPECT_TRUE(WaitUntil([&] {
    return h.fake.calls.contains(QStringLiteral("RenameFolder:4:Beach"));
  })) << h.fake.calls.join(',').toStdString();
}

TEST(CollectionsPanelQmlTest, NewChildCollectionOpensADraftRowAndRevealsTheCreatedFolder) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);

  h.Invoke("beginCreateCollection", h.fake.UiOf(2));
  // The parent chain opens so the draft row is shown under A1.
  EXPECT_TRUE(h.RowShown(2));
  QQuickItem* field = nullptr;
  ASSERT_TRUE(WaitUntil([&] {
    field = FindItem(h.window->contentItem(), QStringLiteral("collectionDraftField"));
    return field != nullptr && field->hasActiveFocus();
  }));
  field->setProperty("text", QStringLiteral("  Day 2 "));
  h.Key(Qt::Key_Return);

  ASSERT_TRUE(WaitUntil([&] {
    return h.fake.calls.contains(QStringLiteral("CreateFolderIn:2:Day 2"));
  })) << h.fake.calls.join(',').toStdString();
  EXPECT_TRUE(WaitUntil([&] { return h.RowShown(500); }));
  EXPECT_EQ(FindItem(h.window->contentItem(), QStringLiteral("collectionDraftRow")), nullptr);
  // The selection does not move to the new folder.
  EXPECT_EQ(h.fake.CurrentFolderId(), 0u);
}

TEST(CollectionsPanelQmlTest, DragMovesACollectionAndRefusesItsOwnSubtree) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);
  h.SetExpanded(1, true);
  ASSERT_TRUE(h.RowShown(2));

  h.Drag(h.Row(2), h.Row(4));
  ASSERT_TRUE(h.fake.calls.contains(QStringLiteral("MoveFolder:2:4")))
      << h.fake.calls.join(',').toStdString();
  EXPECT_EQ(h.fake.ParentOf(2), 4u);
  // The destination opens so the moved folder stays in view.
  EXPECT_TRUE(WaitUntil([&] { return h.RowShown(2); }));

  // B onto its own child A1: no move.
  h.fake.calls.clear();
  h.Drag(h.Row(4), h.Row(2));
  EXPECT_FALSE(h.fake.calls.join(',').contains(QStringLiteral("MoveFolder")))
      << h.fake.calls.join(',').toStdString();
  EXPECT_EQ(h.fake.ParentOf(4), 0u);
}

// Issue #757: local files dropped on a row import into that row's collection.
TEST(CollectionsPanelQmlTest, FileDropOnARowNamesThatCollection) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr);
  const QList<QUrl> files{QUrl::fromLocalFile(QStringLiteral("C:/photos/a.ARW")),
                          QUrl::fromLocalFile(QStringLiteral("C:/photos/trip"))};

  // Disabled (an import runs or an agent holds control): no highlight, no drop.
  EXPECT_FALSE(h.DropUrls(h.Row(1), files));
  EXPECT_EQ(h.LastDropFolderId(), -1);

  h.panel->setProperty("fileDropEnabled", true);
  ProcessEvents(20);
  // A non-current collection: the drop names that collection, not the current one.
  EXPECT_TRUE(h.DropUrls(h.Row(1), files));
  EXPECT_EQ(h.LastDropFolderId(), static_cast<int>(h.fake.UiOf(1)));
  EXPECT_EQ(h.window->property("lastDropUrls").toList().size(), 2);
  EXPECT_FALSE(h.Row(1)->property("dropTarget").toBool());
  // Copy, never Move: a Move answer lets the file manager delete the dropped files.
  EXPECT_EQ(h.last_drop_action, Qt::CopyAction);

  // All Photos.
  h.ClearLastDrop();
  EXPECT_TRUE(h.DropUrls(h.Row(0), files));
  EXPECT_EQ(h.LastDropFolderId(), 0);

  // A web link is not a local file.
  h.ClearLastDrop();
  EXPECT_FALSE(h.DropUrls(h.Row(4), {QUrl(QStringLiteral("https://example.com/a.jpg"))}));
  EXPECT_EQ(h.LastDropFolderId(), -1);
}

TEST(CollectionsPanelQmlTest, FileDropOnTheDraftRowIsRefused) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);
  h.panel->setProperty("fileDropEnabled", true);
  h.Invoke("beginCreateCollection", h.fake.UiOf(4));
  QQuickItem* draft = nullptr;
  ASSERT_TRUE(WaitUntil([&] {
    draft = FindItem(h.window->contentItem(), QStringLiteral("collectionDraftRow"));
    return draft != nullptr && draft->height() > 0.0;
  }));
  EXPECT_FALSE(h.DropUrls(draft, {QUrl::fromLocalFile(QStringLiteral("C:/photos/a.ARW"))}));
  EXPECT_EQ(h.LastDropFolderId(), -1);
}

TEST(CollectionsPanelQmlTest, AddToMenuListsExportQueueThenABoundedFilterableTree) {
  std::vector<FakeFolderController::Node> nodes;
  for (uint i = 1; i <= 30; ++i) {
    nodes.push_back({i, 0, QStringLiteral("Collection %1").arg(i, 2, 10, QLatin1Char('0'))});
  }
  nodes.push_back({31, 7, QStringLiteral("Nested")});
  Harness h(nodes, /*current_element=*/5);
  ASSERT_TRUE(QMetaObject::invokeMethod(h.window, "createMenu"));
  ProcessEvents(50);
  auto* menu = qvariant_cast<QObject*>(h.window->property("menuItem"));
  ASSERT_NE(menu, nullptr) << h.warnings.join('\n').toStdString();

  // Main menu: Rating, one "Add to" entry, separator; no per-collection rows.
  EXPECT_EQ(menu->property("count").toInt(), 3);
  auto* add_to = menu->findChild<QObject*>(QStringLiteral("imageContextAddToMenu"));
  ASSERT_NE(add_to, nullptr);
  EXPECT_EQ(add_to->property("title").toString(), QStringLiteral("Add to"));
  EXPECT_EQ(add_to->property("count").toInt(), 3);
  QQuickItem* first = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(add_to, "itemAt", Q_RETURN_ARG(QQuickItem*, first),
                                        Q_ARG(int, 0)));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->objectName(), QStringLiteral("imageContextAddToExportQueue"));
  EXPECT_EQ(first->property("text").toString(), QStringLiteral("Export Queue"));

  auto* list   = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionList"));
  auto* filter = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionFilter"));
  ASSERT_NE(list, nullptr);
  ASSERT_NE(filter, nullptr);
  const int row_height = AppTheme::Instance().collectionRowHeight();
  EXPECT_EQ(list->property("count").toInt(), 32);  // root, 30 collections, Nested
  EXPECT_TRUE(add_to->property("filterVisible").toBool());

  // Open the menus as a user does, then check the scrolling list and the disabled rows.
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "open"));
  ASSERT_TRUE(WaitUntil([&] { return menu->property("opened").toBool(); }));
  ASSERT_TRUE(QMetaObject::invokeMethod(add_to, "open"));
  ASSERT_TRUE(WaitUntil([&] { return add_to->property("opened").toBool(); }));
  ProcessEvents(50);
  EXPECT_TRUE(filter->isVisible());
  EXPECT_NEAR(list->height(), AppTheme::Instance().collectionMenuMaxRows() * row_height, 0.5);
  EXPECT_GT(list->property("contentHeight").toReal(), list->height());

  auto* root_row = FindItem(list, QStringLiteral("imageContextCollection_0"));
  auto* current  = FindItem(list, QStringLiteral("imageContextCollection_5"));
  ASSERT_NE(root_row, nullptr);
  ASSERT_NE(current, nullptr);
  EXPECT_FALSE(root_row->property("targetEnabled").toBool());
  EXPECT_FALSE(current->property("targetEnabled").toBool());

  // Clicking a collection row reports its ui folder id.
  auto* target = FindItem(list, QStringLiteral("imageContextCollection_3"));
  ASSERT_NE(target, nullptr);
  EXPECT_TRUE(target->property("targetEnabled").toBool());
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(target));
  ASSERT_TRUE(WaitUntil([&] { return h.window->property("lastMenuSignal").toString() == "collection"; }));
  EXPECT_EQ(h.window->property("lastCollectionId").toUInt(), h.fake.UiOf(3));

  // The filter keeps matches and their parents.
  add_to->setProperty("filterText", QStringLiteral("nested"));
  ProcessEvents(30);
  EXPECT_EQ(list->property("count").toInt(), 3);  // root, Collection 07, Nested
  add_to->setProperty("filterText", QStringLiteral("Collection 1"));
  ProcessEvents(30);
  EXPECT_EQ(list->property("count").toInt(), 11);  // root and Collection 10 to 19

  ASSERT_TRUE(QMetaObject::invokeMethod(first, "triggered"));
  ProcessEvents(30);
  EXPECT_EQ(h.window->property("lastMenuSignal").toString(), QStringLiteral("export"));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

TEST(CollectionsPanelQmlTest, AddToMenuWithoutCollectionsShowsTheEmptyRow) {
  Harness h({});
  ASSERT_TRUE(QMetaObject::invokeMethod(h.window, "createMenu"));
  ProcessEvents(50);
  auto* menu = qvariant_cast<QObject*>(h.window->property("menuItem"));
  ASSERT_NE(menu, nullptr);
  auto* add_to = menu->findChild<QObject*>(QStringLiteral("imageContextAddToMenu"));
  ASSERT_NE(add_to, nullptr);
  auto* list   = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionList"));
  auto* empty  = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionEmpty"));
  auto* filter = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionFilter"));
  ASSERT_NE(list, nullptr);
  ASSERT_NE(empty, nullptr);
  ASSERT_NE(filter, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "open"));
  ASSERT_TRUE(WaitUntil([&] { return menu->property("opened").toBool(); }));
  ASSERT_TRUE(QMetaObject::invokeMethod(add_to, "open"));
  ASSERT_TRUE(WaitUntil([&] { return add_to->property("opened").toBool(); }));
  EXPECT_FALSE(list->property("visible").toBool());
  EXPECT_TRUE(empty->property("visible").toBool());
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("No collections yet"));
  EXPECT_FALSE(filter->property("visible").toBool());
}

// Mounted Folder Plan, MF6 (#988): each mounted row shows the link icon after the name, and an
// offline mount shows Offline in the count position.
TEST(CollectionsPanelQmlTest, MountedRowsShowTheLinkIconAndOfflineInTheCountPosition) {
  Harness h(MountTree());
  ASSERT_NE(h.panel, nullptr);
  h.SetExpanded(2, true);
  h.fake.SetImageCounts({{0, 12}, {1, 3}, {2, 5}, {3, 4}, {4, 9}});

  for (const uint element : {2u, 3u, 4u}) {
    auto* icon =
        FindItem(h.window->contentItem(), QStringLiteral("collectionMountIcon_%1").arg(element));
    ASSERT_NE(icon, nullptr) << element;
    EXPECT_TRUE(WaitUntil([&] { return icon->isVisible(); })) << element;
  }
  auto* ordinary_icon = FindItem(h.window->contentItem(), QStringLiteral("collectionMountIcon_1"));
  ASSERT_NE(ordinary_icon, nullptr);
  EXPECT_FALSE(ordinary_icon->isVisible());

  auto* card_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_4"));
  auto* archive_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_2"));
  ASSERT_NE(card_count, nullptr);
  ASSERT_NE(archive_count, nullptr);
  EXPECT_EQ(card_count->property("text").toString(), QStringLiteral("Offline"));
  EXPECT_EQ(archive_count->property("text").toString(), QStringLiteral("5"));
  // The icon follows the name: it sits left of the count. The row layout places a label that
  // turns visible on its next polish.
  auto* archive_icon = FindItem(h.window->contentItem(), QStringLiteral("collectionMountIcon_2"));
  EXPECT_TRUE(WaitUntil([&] {
    return archive_icon->mapToItem(h.Row(2), QPointF(0, 0)).x() <
           archive_count->mapToItem(h.Row(2), QPointF(0, 0)).x();
  })) << "icon x "
      << archive_icon->mapToItem(h.Row(2), QPointF(0, 0)).x() << " count x "
      << archive_count->mapToItem(h.Row(2), QPointF(0, 0)).x();
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// The menu of a mount root has the mount commands and no structure command; Locate Folder is
// enabled only for an offline mount, and Remove Missing Photos only with missing photos.
TEST(CollectionsPanelQmlTest, MountRootMenuHasMountCommandsAndNoStructureCommand) {
  Harness h(MountTree());
  ASSERT_NE(h.panel, nullptr);
  auto* root_menu     = h.window->findChild<QObject*>(QStringLiteral("collectionMountRootMenu"));
  auto* ordinary_menu = h.window->findChild<QObject*>(QStringLiteral("collectionRowMenu"));
  auto* folder_menu = h.window->findChild<QObject*>(QStringLiteral("collectionMountedFolderMenu"));
  ASSERT_NE(root_menu, nullptr);
  ASSERT_NE(ordinary_menu, nullptr);
  ASSERT_NE(folder_menu, nullptr);

  const auto open_menu_of = [&](uint element) {
    QVariant row;
    for (const auto& value : h.panel->property("folderRows").toList()) {
      if (value.toMap().value("elementId").toUInt() == element) {
        row = value;
      }
    }
    ASSERT_TRUE(row.isValid());
    ASSERT_TRUE(QMetaObject::invokeMethod(h.panel, "openRowMenu", Q_ARG(QVariant, row)));
    ProcessEvents(40);
  };

  open_menu_of(2);
  ASSERT_TRUE(WaitUntil([&] { return root_menu->property("visible").toBool(); }));
  EXPECT_FALSE(ordinary_menu->property("visible").toBool());
  EXPECT_TRUE(MenuItem(root_menu, "collectionMenuSynchronize")->property("enabled").toBool());
  EXPECT_FALSE(MenuItem(root_menu, "collectionMenuLocateFolder")->property("enabled").toBool());
  EXPECT_TRUE(MenuItem(root_menu, "collectionMenuShowInFileManager")->property("enabled").toBool());
  EXPECT_TRUE(
      MenuItem(root_menu, "collectionMenuRemoveMissingPhotos")->property("enabled").toBool());
  EXPECT_TRUE(MenuItem(root_menu, "collectionMenuUnmount")->property("enabled").toBool());
  EXPECT_EQ(MenuItem(root_menu, "collectionMenuRename"), nullptr);
  EXPECT_EQ(MenuItem(root_menu, "collectionMenuDelete"), nullptr);
  EXPECT_EQ(MenuItem(root_menu, "collectionMenuNewChild"), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(MenuItem(root_menu, "collectionMenuUnmount"), "triggered"));
  ProcessEvents(30);
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("Unmount:2")))
      << h.fake.calls.join(',').toStdString();
  ASSERT_TRUE(QMetaObject::invokeMethod(root_menu, "close"));
  ProcessEvents(60);

  // The offline mount without missing photos.
  open_menu_of(4);
  ASSERT_TRUE(WaitUntil([&] { return root_menu->property("visible").toBool(); }));
  EXPECT_FALSE(MenuItem(root_menu, "collectionMenuSynchronize")->property("enabled").toBool());
  EXPECT_TRUE(MenuItem(root_menu, "collectionMenuLocateFolder")->property("enabled").toBool());
  EXPECT_FALSE(
      MenuItem(root_menu, "collectionMenuShowInFileManager")->property("enabled").toBool());
  EXPECT_FALSE(
      MenuItem(root_menu, "collectionMenuRemoveMissingPhotos")->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(root_menu, "close"));
  ProcessEvents(60);

  // A mounted folder below the root: Show in File Manager only.
  h.SetExpanded(2, true);
  open_menu_of(3);
  ASSERT_TRUE(WaitUntil([&] { return folder_menu->property("visible").toBool(); }));
  EXPECT_FALSE(root_menu->property("visible").toBool());
  EXPECT_NE(MenuItem(folder_menu, "collectionMenuShowMountedFolder"), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(folder_menu, "close"));
  ProcessEvents(60);

  // An ordinary row keeps its menu and gets Mount Folder Here (issue #1008).
  open_menu_of(1);
  ASSERT_TRUE(WaitUntil([&] { return ordinary_menu->property("visible").toBool(); }));
  ASSERT_NE(MenuItem(ordinary_menu, "collectionMenuMountFolder"), nullptr);
  EXPECT_EQ(MenuItem(ordinary_menu, "collectionMenuMountFolder")->property("text").toString(),
            QStringLiteral("Mount Folder Here..."));
  EXPECT_TRUE(MenuItem(ordinary_menu, "collectionMenuRename")->property("enabled").toBool());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

TEST(CollectionsPanelQmlTest, FileDropOnAMountedRowIsRefusedAndAnOrdinaryRowTakesIt) {
  Harness h(MountTree());
  ASSERT_NE(h.panel, nullptr);
  h.panel->setProperty("fileDropEnabled", true);
  h.SetExpanded(2, true);
  ProcessEvents(20);
  const QList<QUrl> files{QUrl::fromLocalFile(QStringLiteral("C:/photos/a.ARW"))};

  for (const uint element : {2u, 3u}) {
    h.ClearLastDrop();
    EXPECT_FALSE(h.DropUrls(h.Row(element), files)) << element;
    EXPECT_EQ(h.LastDropFolderId(), -1) << element;
  }
  h.ClearLastDrop();
  EXPECT_TRUE(h.DropUrls(h.Row(1), files));
  EXPECT_EQ(h.LastDropFolderId(), static_cast<int>(h.fake.UiOf(1)));
}

TEST(CollectionsPanelQmlTest, MountedRowIsNotDraggedAndTakesNoDraggedRow) {
  Harness h(MountTree());
  ASSERT_NE(h.panel, nullptr);
  // A mounted row does not move.
  h.Drag(h.Row(2), h.Row(1));
  EXPECT_FALSE(h.fake.calls.join(',').contains(QStringLiteral("MoveFolder")))
      << h.fake.calls.join(',').toStdString();
  // An ordinary row cannot be dropped on a mounted row.
  h.Drag(h.Row(1), h.Row(4));
  EXPECT_FALSE(h.fake.calls.join(',').contains(QStringLiteral("MoveFolder")))
      << h.fake.calls.join(',').toStdString();
  // F2 and Delete do nothing on a mounted row.
  h.Invoke("beginRename", h.panel->property("folderRows").toList().at(2));
  EXPECT_EQ(h.panel->property("renamingElementId").toInt(), -1);
}

// Issue #1007: a reload the user did not start (a count refresh, a row added by a mount scan)
// keeps the scroll position of the tree.
TEST(CollectionsPanelQmlTest, ReloadWithoutAUserActionKeepsTheScrollPosition) {
  Harness h(LongTree());
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  auto* tree = h.Tree();
  ASSERT_NE(tree, nullptr);
  tree->setProperty("contentY", 500.0);
  ProcessEvents(50);
  const double kept = tree->property("contentY").toDouble();
  ASSERT_DOUBLE_EQ(kept, 500.0);
  // F20 (element 30) is in the view; its place on the screen must not move.
  const auto scene_y = [&h] { return h.Row(30)->mapToScene(QPointF(0, 0)).y(); };
  ASSERT_NE(h.Row(30), nullptr);
  const double kept_scene_y = scene_y();

  h.fake.SetImageCounts({{0, 90}, {10, 3}, {40, 7}});
  ProcessEvents(80);
  EXPECT_DOUBLE_EQ(tree->property("contentY").toDouble(), kept);
  EXPECT_DOUBLE_EQ(scene_y(), kept_scene_y);

  // A mount scan adds a row under the collapsed mount root and reassigns every ui id.
  auto nodes = LongTree();
  nodes.push_back({7, 5, QStringLiteral("Day2"), 1, false, false, 0});
  h.fake.SetTree(std::move(nodes), 0);
  ProcessEvents(80);
  EXPECT_FALSE(h.RowShown(7));
  ASSERT_NE(h.Row(30), nullptr);
  // A new model resets the ListView, which then places rows it has not created at estimated
  // positions, so contentY is no stable coordinate here: the rows keep their place on the
  // screen.
  EXPECT_DOUBLE_EQ(scene_y(), kept_scene_y);
  ASSERT_NE(h.Row(40), nullptr);
  EXPECT_DOUBLE_EQ(h.Row(40)->mapToScene(QPointF(0, 0)).y(),
                   kept_scene_y + 10.0 * AppTheme::Instance().collectionRowHeight());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1007: a selection from outside the tree scrolls the tree to the selected row.
TEST(CollectionsPanelQmlTest, SelectionFromOutsideScrollsToTheSelectedRow) {
  Harness h(LongTree());
  ASSERT_NE(h.panel, nullptr);
  auto* tree = h.Tree();
  ASSERT_NE(tree, nullptr);
  ASSERT_DOUBLE_EQ(tree->property("contentY").toDouble(), 0.0);

  h.fake.SelectFolder(h.fake.UiOf(65));
  ASSERT_TRUE(WaitUntil([&] { return h.RowShown(65); }));
  ProcessEvents(50);
  auto*        row       = h.Row(65);
  const double content_y = tree->property("contentY").toDouble();
  ASSERT_NE(row, nullptr);
  EXPECT_GT(content_y, 0.0);
  EXPECT_GE(row->y(), content_y);
  EXPECT_LE(row->y() + row->height(), content_y + tree->height() + 0.5);

  // A count refresh afterwards does not move the tree.
  h.fake.SetImageCounts({{65, 4}});
  ProcessEvents(80);
  EXPECT_DOUBLE_EQ(tree->property("contentY").toDouble(), content_y);
}

namespace {

/// The display row of @p element in the panel, or an empty map.
auto PanelRow(Harness& h, uint element) -> QVariantMap {
  for (const auto& value : h.panel->property("folderRows").toList()) {
    const auto row = value.toMap();
    if (!row.value("isDraft").toBool() && row.value("elementId").toUInt() == element) {
      return row;
    }
  }
  return {};
}

/// Clicks Mount Folder and accepts the folder dialog with @p directory.
void OpenFolderWith(Harness& h, const QString& directory) {
  auto* button = FindItem(h.window->contentItem(), QStringLiteral("collectionsOpenFolderButton"));
  auto* dialog = h.panel->findChild<QObject*>(QStringLiteral("collectionMountFolderDialog"));
  ASSERT_NE(button, nullptr);
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(button->isEnabled());
  ASSERT_TRUE(QMetaObject::invokeMethod(h.window, "useQuickDialog",
                                        Q_ARG(QVariant, QVariant::fromValue(dialog))));
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(button));
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("visible").toBool(); }));
  const QUrl url = QUrl::fromLocalFile(directory);
  dialog->setProperty("currentFolder", url);
  dialog->setProperty("selectedFolder", url);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "accept"));
  ASSERT_TRUE(WaitUntil([&] { return !dialog->property("visible").toBool(); }));
  ProcessEvents(60);
}

}  // namespace

// Issue #1008: Mount Folder at the foot of the panel mounts the chosen directory below the
// selected collection: below the library root (tree depth 1), or inside an ordinary collection.
TEST(CollectionsPanelQmlTest, OpenFolderMountsTheDirectoryBelowTheSelectedCollection) {
  Harness h(SmallTree(), /*current_element=*/0);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir first_dir;
  QTemporaryDir second_dir;
  ASSERT_TRUE(first_dir.isValid());
  ASSERT_TRUE(second_dir.isValid());

  OpenFolderWith(h, first_dir.path());
  ASSERT_EQ(h.fake.calls.filter(QStringLiteral("MountFolder:")).size(), 1)
      << h.fake.calls.join(',').toStdString();
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("MountFolder:0:%1").arg(first_dir.path())))
      << h.fake.calls.join(',').toStdString();
  const QVariantMap root_mount = PanelRow(h, 500);
  ASSERT_FALSE(root_mount.isEmpty());
  EXPECT_TRUE(root_mount.value("mountRoot").toBool());
  EXPECT_EQ(root_mount.value("treeDepth").toInt(), 1);

  h.fake.SelectFolder(h.fake.UiOf(1));
  ProcessEvents(40);
  OpenFolderWith(h, second_dir.path());
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("MountFolder:1:%1").arg(second_dir.path())))
      << h.fake.calls.join(',').toStdString();
  const QVariantMap inner_mount = PanelRow(h, 501);
  ASSERT_FALSE(inner_mount.isEmpty());
  EXPECT_TRUE(inner_mount.value("mountRoot").toBool());
  EXPECT_EQ(inner_mount.value("treeDepth").toInt(), 2);
  EXPECT_EQ(inner_mount.value("parentElementId").toUInt(), 1u);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1008: Mount Folder and the two halves of the import button follow the selected row; a
// mounted selection, or a backend that is not interactive, disables all three.
TEST(CollectionsPanelQmlTest, FootActionsAreDisabledOnAMountedSelection) {
  Harness h(MountTree(), /*current_element=*/1);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  auto* content     = h.window->contentItem();
  auto* open_folder = FindItem(content, QStringLiteral("collectionsOpenFolderButton"));
  auto* images      = FindItem(content, QStringLiteral("collectionsImportImagesButton"));
  auto* folder      = FindItem(content, QStringLiteral("collectionsImportFolderButton"));
  ASSERT_NE(open_folder, nullptr);
  ASSERT_NE(images, nullptr);
  ASSERT_NE(folder, nullptr);
  const auto all_enabled = [&](bool expected) {
    return open_folder->isEnabled() == expected && images->isEnabled() == expected &&
           folder->isEnabled() == expected;
  };

  EXPECT_TRUE(all_enabled(true));
  EXPECT_EQ(images->property("text").toString(), QStringLiteral("Import Images"));
  EXPECT_EQ(folder->property("text").toString(), QStringLiteral("Import Folder"));
  EXPECT_EQ(open_folder->property("text").toString(), QStringLiteral("Mount Folder"));
  // The halves split the width, sit on one row, and hold their labels without eliding.
  EXPECT_NEAR(images->width(), folder->width(), 1.0);
  for (auto* segment : {images, folder}) {
    auto* label = qvariant_cast<QQuickItem*>(segment->property("contentItem"));
    ASSERT_NE(label, nullptr);
    EXPECT_LE(label->implicitWidth(), segment->property("availableWidth").toReal())
        << segment->objectName().toStdString();
    // One fill shape, clipped at the inner edge: a translucent disabled fill has no overlap
    // that draws a darker strip.
    auto* background = qvariant_cast<QQuickItem*>(segment->property("background"));
    ASSERT_NE(background, nullptr);
    EXPECT_TRUE(background->clip());
    ASSERT_EQ(background->childItems().size(), 1) << segment->objectName().toStdString();
    auto* fill = background->childItems().at(0);
    EXPECT_DOUBLE_EQ(fill->width(), background->width() + 8.0);
  }
  // The leading half overhangs its right edge and the trailing half its left edge.
  EXPECT_DOUBLE_EQ(
      qvariant_cast<QQuickItem*>(images->property("background"))->childItems().at(0)->x(), 0.0);
  EXPECT_DOUBLE_EQ(
      qvariant_cast<QQuickItem*>(folder->property("background"))->childItems().at(0)->x(), -8.0);
  EXPECT_DOUBLE_EQ(images->mapToScene(QPointF(0, 0)).y(), folder->mapToScene(QPointF(0, 0)).y());
  EXPECT_LT(open_folder->mapToScene(QPointF(0, 0)).y(), images->mapToScene(QPointF(0, 0)).y());

  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(images));
  EXPECT_TRUE(WaitUntil([&] {
    return h.window->property("lastImportSignal").toString() == QStringLiteral("images");
  }));
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(folder));
  EXPECT_TRUE(WaitUntil([&] {
    return h.window->property("lastImportSignal").toString() == QStringLiteral("folder");
  }));

  // The mount root and a mounted folder below it.
  h.SetExpanded(2, true);
  for (const uint element : {2u, 3u}) {
    h.fake.SelectFolder(h.fake.UiOf(element));
    EXPECT_TRUE(WaitUntil([&] { return all_enabled(false); })) << element;
  }
  h.window->setProperty("lastImportSignal", QString());
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(images));
  ProcessEvents(40);
  EXPECT_TRUE(h.window->property("lastImportSignal").toString().isEmpty());

  // The library root takes both actions again.
  h.fake.SelectFolder(0);
  EXPECT_TRUE(WaitUntil([&] { return all_enabled(true); }));
  h.panel->setProperty("backendInteractive", false);
  EXPECT_TRUE(WaitUntil([&] { return all_enabled(false); }));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1008: the row menu of the library root has Mount Folder Here.
TEST(CollectionsPanelQmlTest, RootRowMenuHasOpenFolderHere) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);
  auto* menu = h.window->findChild<QObject*>(QStringLiteral("collectionRowMenu"));
  ASSERT_NE(menu, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(h.panel, "openRowMenu", Q_ARG(QVariant, QVariant(PanelRow(h, 0)))));
  ASSERT_TRUE(WaitUntil([&] { return menu->property("visible").toBool(); }));
  auto* item = MenuItem(menu, "collectionMenuMountFolder");
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->property("text").toString(), QStringLiteral("Mount Folder Here..."));
  EXPECT_TRUE(item->property("enabled").toBool());
}

namespace {

/// Elements of the rows of the open "Add to" collection list, in list order.
auto AddToElements(Harness& h) -> std::vector<uint> {
  std::vector<uint> elements;
  EXPECT_TRUE(QMetaObject::invokeMethod(h.window, "createMenu"));
  ProcessEvents(50);
  auto* menu = qvariant_cast<QObject*>(h.window->property("menuItem"));
  EXPECT_NE(menu, nullptr);
  if (menu == nullptr) {
    return elements;
  }
  auto* add_to = menu->findChild<QObject*>(QStringLiteral("imageContextAddToMenu"));
  EXPECT_NE(add_to, nullptr);
  if (add_to == nullptr) {
    return elements;
  }
  for (const auto& value : add_to->property("shownRows").toList()) {
    elements.push_back(value.toMap().value("elementId").toUInt());
  }
  return elements;
}

}  // namespace

// Issue #1008: "Add to" lists the root anchor and the ordinary collections; a mount and its
// folders are left out, and the empty text counts the listed collections only.
TEST(CollectionsPanelQmlTest, AddToMenuListsOrdinaryCollectionsOnly) {
  {
    // Ordinary Trips (1) and Walks (5); the mount Archive (2) with Day1 (3) and Day2 (6).
    Harness h({{1, 0, QStringLiteral("Trips")},
               {5, 0, QStringLiteral("Walks")},
               {2, 0, QStringLiteral("Archive"), 1, true, false, 0},
               {3, 2, QStringLiteral("Day1"), 1, false, false, 0},
               {6, 2, QStringLiteral("Day2"), 1, false, false, 0}});
    ASSERT_NE(h.panel, nullptr);
    EXPECT_EQ(AddToElements(h), (std::vector<uint>{0, 1, 5}));
    auto* menu   = qvariant_cast<QObject*>(h.window->property("menuItem"));
    auto* add_to = menu->findChild<QObject*>(QStringLiteral("imageContextAddToMenu"));
    EXPECT_EQ(add_to->property("collectionCount").toInt(), 2);
  }
  {
    Harness h({{2, 0, QStringLiteral("Archive"), 1, true, false, 0},
               {3, 2, QStringLiteral("Day1"), 1, false, false, 0}});
    ASSERT_NE(h.panel, nullptr);
    EXPECT_EQ(AddToElements(h), (std::vector<uint>{0}));
    auto* menu   = qvariant_cast<QObject*>(h.window->property("menuItem"));
    auto* add_to = menu->findChild<QObject*>(QStringLiteral("imageContextAddToMenu"));
    auto* empty  = add_to->findChild<QQuickItem*>(QStringLiteral("imageContextCollectionEmpty"));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(add_to->property("collectionCount").toInt(), 0);
    EXPECT_EQ(empty->property("text").toString(), QStringLiteral("No collections yet"));
  }
}

}  // namespace alcedo::ui::test

#include "collections_panel_qml_test.moc"
