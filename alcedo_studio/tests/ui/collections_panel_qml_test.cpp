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
                               {"imageCount", count_of(0)}});
    for (const auto& node : nodes_) {
      rows.push_back(QVariantMap{{"folderId", UiOf(node.element_)},
                                 {"elementId", node.element_},
                                 {"parentFolderId", UiOf(node.parent_element_)},
                                 {"parentElementId", node.parent_element_},
                                 {"name", node.name_},
                                 {"imageCount", count_of(node.element_)}});
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
};

constexpr char kHarness[] = R"(
import QtQuick
import QtQuick.Controls

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

    function secondaryButtonFill(enabled, hovered, pressed) {
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

}  // namespace alcedo::ui::test

#include "collections_panel_qml_test.moc"
