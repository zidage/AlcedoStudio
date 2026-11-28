//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file collections_panel_qml_test.cpp
/// @brief Collections sidebar tree, image-menu "Add to" sub-menu (issue #732), and the Import
///        Folder dialog and its steps (issue #1078).
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
  Q_PROPERTY(bool libraryRootSelected READ LibraryRootSelected NOTIFY FolderSelectionChanged)
  Q_PROPERTY(qint64 allPhotosCount READ AllPhotosCount NOTIFY FoldersChanged)
  Q_PROPERTY(QVariantMap arrivedVolume READ ArrivedVolume NOTIFY ArrivedVolumeChanged)

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
    /// The volume fields of a mounted row (#1078).
    QString volume_key_;
    QString volume_name_;
    QString mount_point_;
  };

  void SetTree(std::vector<Node> nodes, uint current_element) {
    nodes_           = std::move(nodes);
    current_element_ = current_element;
    Rebuild();
  }

  [[nodiscard]] auto Folders() const -> QVariantList { return rows_; }
  [[nodiscard]] auto CurrentFolderId() const -> uint { return UiOf(current_element_); }
  [[nodiscard]] auto LibraryRootSelected() const -> bool { return library_root_selected_; }
  [[nodiscard]] auto AllPhotosCount() const -> qint64 { return all_photos_count_; }
  [[nodiscard]] auto ArrivedVolume() const -> QVariantMap { return arrived_volume_; }
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
    current_element_       = ElementOf(folderUiId);
    library_root_selected_ = false;
    calls << QStringLiteral("SelectFolder:%1").arg(current_element_);
    emit FolderSelectionChanged();
    emit folderSelectionChanged();
  }

  Q_INVOKABLE void SelectLibraryRoot() {
    current_element_       = 0;
    library_root_selected_ = true;
    calls << QStringLiteral("SelectLibraryRoot");
    emit FolderSelectionChanged();
    emit folderSelectionChanged();
  }

  /// A volume that arrived with no mount, as FolderController::SetArrivedVolume publishes it.
  void SetArrivedVolume(const QString& name, const QString& root, bool has_dcim) {
    arrived_volume_ = QVariantMap{
        {"name", name},
        {"mountPoint", root.left(2)},
        {"hasDcim", has_dcim},
        {"rootUrl", QUrl::fromLocalFile(root)},
        {"dcimUrl", has_dcim ? QUrl::fromLocalFile(root + QStringLiteral("/DCIM")) : QUrl{}}};
    emit ArrivedVolumeChanged();
  }

  Q_INVOKABLE void DismissArrivedVolume() {
    calls << QStringLiteral("DismissArrivedVolume");
    arrived_volume_.clear();
    emit ArrivedVolumeChanged();
  }

  /// FolderController::TakeArrivedVolume: closes the card; true while the volume of the card is
  /// at its mount point (arrived_volume_present).
  Q_INVOKABLE bool TakeArrivedVolume() {
    calls << QStringLiteral("TakeArrivedVolume");
    arrived_volume_.clear();
    emit ArrivedVolumeChanged();
    return arrived_volume_present;
  }

  bool arrived_volume_present = true;

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

  /// Adds a mount root named after the directory below the library root, as a finished mount
  /// creation does (#1076).
  Q_INVOKABLE void MountFolder(const QUrl& directory) {
    const uint    parent = 0;
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
  void SetImageCounts(std::map<uint, qint64> counts, qint64 all_photos = -1) {
    counts_           = std::move(counts);
    all_photos_count_ = all_photos;
    Publish();
    emit FoldersChanged();
  }

 signals:
  void FoldersChanged();
  void FolderSelectionChanged();
  void folderSelectionChanged();
  void ArrivedVolumeChanged();

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
      rows.push_back(
          QVariantMap{{"folderId", UiOf(node.element_)},
                      {"elementId", node.element_},
                      {"parentFolderId", UiOf(node.parent_element_)},
                      {"parentElementId", node.parent_element_},
                      {"name", node.name_},
                      {"imageCount", count_of(node.element_)},
                      {"mountId", node.mount_id_},
                      {"mountRoot", node.mount_root_},
                      {"mountOffline", node.mount_offline_},
                      {"mountMissingCount", node.mount_missing_},
                      {"mountVolumeKey", node.volume_key_},
                      {"mountVolumeName", node.volume_name_},
                      {"mountPoint", node.mount_offline_ ? QString{} : node.mount_point_},
                      {"mountVolumeRoot", node.mount_point_},
                      {"structureEditable", node.mount_id_ == 0}});
    }
    rows_ = rows;
  }

  std::vector<Node>      nodes_;
  std::map<uint, uint>   ui_by_element_;
  std::map<uint, qint64> counts_;
  QVariantList           rows_;
  QVariantMap            arrived_volume_;
  qint64                 all_photos_count_      = -1;
  bool                   library_root_selected_ = false;
  uint                 current_element_ = 0;
  uint                 generation_      = 0;
  uint                 next_element_    = 500;
  uint                 next_mount_      = 100;
};

/// ImportExportHandler.folderScan: a scan finishes at once with two RAW files and one JPEG file
/// in one subfolder, on the volume "Card" at F:.
class FakeFolderScan final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool scanning READ Scanning NOTIFY ScanStateChanged)
  Q_PROPERTY(bool scanFinished READ ScanFinished NOTIFY ScanStateChanged)
  Q_PROPERTY(bool folderValid READ FolderValid NOTIFY ScanStateChanged)
  Q_PROPERTY(int fileCount READ FileCount NOTIFY ScanStateChanged)
  Q_PROPERTY(int allowedFileCount READ AllowedFileCount NOTIFY AllowedCategoriesChanged)
  Q_PROPERTY(QVariantList categoryCounts READ CategoryCounts NOTIFY ScanStateChanged)
  Q_PROPERTY(int allowedCategories READ AllowedCategories NOTIFY AllowedCategoriesChanged)
  Q_PROPERTY(int droppedFileCount READ DroppedFileCount NOTIFY ScanStateChanged)
  Q_PROPERTY(QString folderPath READ FolderPath NOTIFY ScanStateChanged)
  Q_PROPERTY(QString currentDirectory READ CurrentDirectory NOTIFY ScanStateChanged)
  Q_PROPERTY(int subfolderCount READ SubfolderCount NOTIFY ScanStateChanged)
  Q_PROPERTY(QString volumeName READ VolumeName NOTIFY ScanStateChanged)
  Q_PROPERTY(QString volumeMountPoint READ VolumeMountPoint NOTIFY ScanStateChanged)

 public:
  QStringList        calls;

  [[nodiscard]] bool Scanning() const { return false; }
  [[nodiscard]] bool ScanFinished() const { return finished_; }
  [[nodiscard]] bool FolderValid() const { return true; }
  [[nodiscard]] int  FileCount() const { return finished_ ? 3 : 0; }
  [[nodiscard]] int  AllowedFileCount() const {
    if (!finished_) {
      return 0;
    }
    return ((allowed_ & 1) != 0 ? 2 : 0) + ((allowed_ & 2) != 0 ? 1 : 0);
  }
  [[nodiscard]] auto CategoryCounts() const -> QVariantList {
    return finished_ ? QVariantList{2, 1, 0, 0, 0, 0} : QVariantList{0, 0, 0, 0, 0, 0};
  }
  [[nodiscard]] int  AllowedCategories() const { return allowed_; }
  [[nodiscard]] int  DroppedFileCount() const { return 0; }
  [[nodiscard]] auto FolderPath() const -> QString { return folder_path_; }
  [[nodiscard]] auto CurrentDirectory() const -> QString { return {}; }
  [[nodiscard]] int  SubfolderCount() const { return finished_ ? 1 : 0; }
  [[nodiscard]] auto VolumeName() const -> QString { return QStringLiteral("Card"); }
  [[nodiscard]] auto VolumeMountPoint() const -> QString { return QStringLiteral("F:"); }

  Q_INVOKABLE void   Start(const QString& url) {
    calls << QStringLiteral("Start:") + url;
    Finish(url);
  }
  Q_INVOKABLE void StartSources(const QStringList& urls) {
    calls << QStringLiteral("StartSources:%1").arg(urls.size());
    Finish(urls.value(0));
  }
  Q_INVOKABLE void Cancel() {
    calls << QStringLiteral("Cancel");
    finished_ = false;
    emit ScanStateChanged();
    emit AllowedCategoriesChanged();
  }
  Q_INVOKABLE void SetCategoryAllowed(int category, bool allowed) {
    allowed_ = allowed ? (allowed_ | (1 << category)) : (allowed_ & ~(1 << category));
    emit AllowedCategoriesChanged();
  }

 signals:
  void ScanStateChanged();
  void AllowedCategoriesChanged();

 private:
  void Finish(const QString& path) {
    folder_path_ = path;
    finished_    = true;
    emit ScanStateChanged();
    emit AllowedCategoriesChanged();
  }

  bool    finished_ = false;
  int     allowed_  = 31;
  QString folder_path_;
};

/// ImportExportHandler: records the import entries of the Import Folder steps.
class FakeImportExport final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* folderScan READ FolderScan CONSTANT)

 public:
  explicit FakeImportExport(FakeFolderScan* scan) : scan_(scan) {}
  [[nodiscard]] auto FolderScan() const -> QObject* { return scan_; }

  QStringList        calls;

  Q_INVOKABLE void   StartFolderImportOfFolderAsUser(const QString& url, int folderUiId) {
    calls << QStringLiteral("StartFolderImportOfFolderAsUser:%1:%2").arg(url).arg(folderUiId);
  }
  Q_INVOKABLE void StartFolderImportToFolderAsUser(int folderUiId) {
    calls << QStringLiteral("StartFolderImportToFolderAsUser:%1").arg(folderUiId);
  }
  Q_INVOKABLE bool DropHoldsFolder(const QStringList&) const { return true; }

 private:
  FakeFolderScan* scan_ = nullptr;
};

/// LibraryBehaviorPreferences: the stored default action of Import Folder (#1077).
class FakeLibraryBehavior final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString importFolderAction READ ImportFolderAction NOTIFY ImportFolderActionChanged)

 public:
  [[nodiscard]] auto ImportFolderAction() const -> QString { return action_; }
  Q_INVOKABLE void   setImportFolderAction(const QString& action) {
    action_ = action;
    emit ImportFolderActionChanged();
  }

 signals:
  void ImportFolderActionChanged();

 private:
  QString action_ = QStringLiteral("ask");
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
    property var flowItem: flowLoader.item
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
            item.importFolderRequested.connect(function(action, folderUrl, startFolderUrl) {
                root.lastImportSignal = "folder:" + action
                root.lastImportFolderUrl = String(folderUrl)
                root.lastImportStartUrl = String(startFolderUrl)
                if (flowLoader.item)
                    flowLoader.item.begin(action, folderUrl, startFolderUrl)
            })
        }
    }

    property string lastImportFolderUrl: ""
    property string lastImportStartUrl: ""

    Loader {
        id: flowLoader
        source: flowSourceUrl
        onLoaded: {
            item.folders = folderFake
            item.importExport = importExportFake
            item.libraryBehavior = behaviorFake
            item.theme = root
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
  FakeFolderScan        scan;
  FakeImportExport      import_export{&scan};
  FakeLibraryBehavior   behavior;
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
    engine.rootContext()->setContextProperty(QStringLiteral("importExportFake"), &import_export);
    engine.rootContext()->setContextProperty(QStringLiteral("behaviorFake"), &behavior);
    engine.rootContext()->setContextProperty(
        QStringLiteral("panelSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/CollectionsPanel.qml")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("menuSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/ImageContextMenu.qml")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("flowSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/ImportFolderFlow.qml")));
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
  /// The All Photos row above the tree: the root row of the folder model.
  auto AllPhotos() -> QQuickItem* {
    return FindItem(window->contentItem(), QStringLiteral("collectionsAllPhotosRow"));
  }
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
  EXPECT_DOUBLE_EQ(h.Row(1)->height(), AppTheme::Instance().collectionsTreeRowHeight());

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

  // From All Photos, Down enters the tree at the Library row; the next Down reaches A.
  h.Key(Qt::Key_Down);
  EXPECT_TRUE(h.fake.LibraryRootSelected());
  h.Key(Qt::Key_Down);
  EXPECT_EQ(h.fake.ElementOf(h.fake.CurrentFolderId()), 1u);
  EXPECT_FALSE(h.fake.LibraryRootSelected());
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

// Issue #733: every row shows the photo count of its collection. Issue #1078: the All Photos row
// shows the count of every photo; the Library row shows the count of the root row, which leaves
// out the mount trees.
TEST(CollectionsPanelQmlTest, RowsShowThePhotoCountInCaptionInk) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr);
  auto* all_photos = h.AllPhotos();
  auto* a_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_1"));
  auto* b_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_4"));
  auto* root_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_0"));
  ASSERT_NE(all_photos, nullptr);
  ASSERT_NE(a_count, nullptr);
  ASSERT_NE(b_count, nullptr);
  ASSERT_NE(root_count, nullptr);
  // No count read yet: no label.
  EXPECT_FALSE(a_count->isVisible());
  EXPECT_EQ(all_photos->property("trailingText").toString(), QString());

  h.fake.SetImageCounts({{0, 40}, {1, 3}, {2, 2}, {3, 1}, {4, 0}}, 1234);
  ASSERT_TRUE(WaitUntil([&] { return a_count->isVisible(); }));
  EXPECT_EQ(all_photos->property("trailingText").toString(), QLocale().toString(1234.0, 'f', 0));
  EXPECT_EQ(root_count->property("text").toString(), QStringLiteral("40"));
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

  // The Library row (#1078). All Photos is the view of every photo, not a container.
  h.ClearLastDrop();
  EXPECT_TRUE(h.DropUrls(h.Row(0), files));
  EXPECT_EQ(h.LastDropFolderId(), 0);
  h.ClearLastDrop();
  EXPECT_FALSE(h.DropUrls(h.AllPhotos(), files));
  EXPECT_EQ(h.LastDropFolderId(), -1);

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
// Issue #1028: a mount root has the folder-symlink icon; a mounted folder and an ordinary
// collection have the folder icon. The icon stands before the name.
TEST(CollectionsPanelQmlTest, MountRootsShowTheSymlinkIconAndOfflineInTheCountPosition) {
  Harness h(MountTree());
  ASSERT_NE(h.panel, nullptr);
  h.SetExpanded(2, true);
  h.fake.SetImageCounts({{0, 12}, {1, 3}, {2, 5}, {3, 4}, {4, 9}});

  const auto icon_of = [&h](uint element) {
    return FindItem(h.window->contentItem(), QStringLiteral("collectionRowIcon_%1").arg(element));
  };
  for (const uint element : {1u, 2u, 3u, 4u}) {
    auto* icon = icon_of(element);
    ASSERT_NE(icon, nullptr) << element;
    EXPECT_TRUE(WaitUntil([&] { return icon->isVisible(); })) << element;
    const bool mount_root = element == 2u || element == 4u;
    EXPECT_EQ(icon->property("source").toUrl().toString(),
              mount_root ? QStringLiteral("qrc:/panel_icons/folder-symlink.svg")
                         : QStringLiteral("qrc:/panel_icons/folder.svg"))
        << element;
  }
  EXPECT_EQ(FindItem(h.window->contentItem(), QStringLiteral("collectionMountIcon_2")), nullptr);

  auto* card_count    = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_4"));
  auto* archive_count = FindItem(h.window->contentItem(), QStringLiteral("collectionCount_2"));
  ASSERT_NE(card_count, nullptr);
  ASSERT_NE(archive_count, nullptr);
  EXPECT_EQ(card_count->property("text").toString(), QStringLiteral("Offline"));
  EXPECT_EQ(archive_count->property("text").toString(), QStringLiteral("5"));
  // The icon sits left of the count. The row layout places a label that turns visible on its
  // next polish.
  auto* archive_icon = icon_of(2);
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
      if (!value.toMap().contains("rowKind") &&
          value.toMap().value("elementId").toUInt() == element) {
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

  // An ordinary row keeps its menu; it has no Mount Folder Here: mounts live in the Disk
  // section (#1076, #1078).
  open_menu_of(1);
  ASSERT_TRUE(WaitUntil([&] { return ordinary_menu->property("visible").toBool(); }));
  EXPECT_EQ(MenuItem(ordinary_menu, "collectionMenuMountFolder"), nullptr);
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
  QVariant archive;
  for (const auto& value : h.panel->property("folderRows").toList()) {
    const auto row = value.toMap();
    if (!row.contains("rowKind") && row.value("elementId").toUInt() == 2u) {
      archive = value;
    }
  }
  ASSERT_TRUE(archive.isValid());
  h.Invoke("beginRename", archive);
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
                   kept_scene_y + 10.0 * AppTheme::Instance().collectionsTreeRowHeight());
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
    if (!row.value("isDraft").toBool() && !row.contains("rowKind") &&
        row.value("elementId").toUInt() == element) {
      return row;
    }
  }
  return {};
}

/// Clicks the Import button at the foot of the panel and waits for its menu.
auto OpenImportMenu(Harness& h) -> QObject* {
  auto* button = FindItem(h.window->contentItem(), QStringLiteral("collectionsImportButton"));
  auto* menu   = h.panel->findChild<QObject*>(QStringLiteral("collectionsImportMenu"));
  EXPECT_NE(button, nullptr);
  EXPECT_NE(menu, nullptr);
  if (button == nullptr || menu == nullptr) {
    return nullptr;
  }
  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(button));
  EXPECT_TRUE(WaitUntil([&] { return menu->property("opened").toBool(); }));
  return menu;
}

/// The Import Folder steps (ImportFolderFlow) of the harness.
auto Flow(Harness& h) -> QObject* {
  return qvariant_cast<QObject*>(h.window->property("flowItem"));
}

/// The Import Folder dialog of the steps.
auto FlowDialog(Harness& h) -> QObject* {
  auto* flow = Flow(h);
  return flow != nullptr ? qvariant_cast<QObject*>(flow->property("dialog")) : nullptr;
}

/// The system folder dialog of the steps, drawn inside the harness window.
auto UseQuickFolderDialog(Harness& h) -> QObject* {
  auto* flow = Flow(h);
  EXPECT_NE(flow, nullptr);
  auto* dialog =
      flow != nullptr ? qvariant_cast<QObject*>(flow->property("folderDialog")) : nullptr;
  EXPECT_NE(dialog, nullptr);
  if (dialog != nullptr) {
    EXPECT_TRUE(QMetaObject::invokeMethod(h.window, "useQuickDialog",
                                          Q_ARG(QVariant, QVariant::fromValue(dialog))));
  }
  return dialog;
}

/// Accepts the open system folder dialog of the steps with @p directory.
void AcceptFolderDialog(QObject* dialog, const QString& directory) {
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("visible").toBool(); }));
  const QUrl url = QUrl::fromLocalFile(directory);
  dialog->setProperty("currentFolder", url);
  dialog->setProperty("selectedFolder", url);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "accept"));
  ASSERT_TRUE(WaitUntil([&] { return !dialog->property("visible").toBool(); }));
  ProcessEvents(60);
}

/// An item of the Import Folder dialog, which draws in the overlay of the window.
auto DialogItem(Harness& h, const QString& object_name) -> QQuickItem* {
  auto* dialog = FlowDialog(h);
  return dialog != nullptr
             ? FindItem(qvariant_cast<QQuickItem*>(dialog->property("contentItem")), object_name)
             : nullptr;
}

/// Chooses Import Folder... in the Import menu.
void ChooseImportFolder(Harness& h) {
  auto* menu = OpenImportMenu(h);
  ASSERT_NE(menu, nullptr);
  auto* item = MenuItem(menu, "collectionsImportFolderItem");
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(item->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(item, "triggered"));
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
  ProcessEvents(40);
}

/// Clicks the + of the Disk section header.
void ClickDiskAdd(Harness& h) {
  auto* button = FindItem(h.window->contentItem(), QStringLiteral("collectionsDiskAddButton"));
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(button->isEnabled());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  ProcessEvents(40);
}

/// The element ids of the shown rows of the destination tree, in order.
auto PickerElements(QObject* picker) -> std::vector<uint> {
  std::vector<uint> elements;
  for (const auto& value : picker->property("shownRows").toList()) {
    elements.push_back(value.toMap().value("elementId").toUInt());
  }
  return elements;
}

// Trips (1) > Kyoto (2) > Day (3), Portraits (4), and the mount Archive (5) with Day1 (6).
auto PickerTree() -> std::vector<FakeFolderController::Node> {
  return {{1, 0, QStringLiteral("Trips")},
          {2, 1, QStringLiteral("Kyoto")},
          {3, 2, QStringLiteral("Day")},
          {4, 0, QStringLiteral("Portraits")},
          {5, 0, QStringLiteral("Archive"), 1, true, false, 0, "vol-d", "Data", "D:"},
          {6, 5, QStringLiteral("Day1"), 1, false, false, 0, "vol-d", "Data", "D:"}};
}

}  // namespace

// Issue #1078: the Import menu has Import Images and Import Folder. Import Images follows the
// selected row, so a mounted selection disables it; Import Folder then imports into Library and
// stays enabled. A backend that is not interactive disables both.
TEST(CollectionsPanelQmlTest, ImportMenuHasTwoItemsAndImportFolderStaysEnabled) {
  Harness h(MountTree(), /*current_element=*/1);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  auto* button = FindItem(h.window->contentItem(), QStringLiteral("collectionsImportButton"));
  ASSERT_NE(button, nullptr);
  EXPECT_EQ(button->property("text").toString(), QStringLiteral("Import"));

  auto* menu = OpenImportMenu(h);
  ASSERT_NE(menu, nullptr);
  auto* images = MenuItem(menu, "collectionsImportImagesItem");
  auto* folder = MenuItem(menu, "collectionsImportFolderItem");
  ASSERT_NE(images, nullptr);
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(MenuItem(menu, "collectionsMountFolderItem"), nullptr);
  EXPECT_EQ(menu->property("count").toInt(), 2);
  EXPECT_EQ(images->property("text").toString(), QStringLiteral("Import Images..."));
  EXPECT_EQ(folder->property("text").toString(), QStringLiteral("Import Folder..."));
  EXPECT_TRUE(images->property("enabled").toBool());
  EXPECT_TRUE(folder->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(images, "triggered"));
  EXPECT_TRUE(WaitUntil([&] {
    return h.window->property("lastImportSignal").toString() == QStringLiteral("images");
  }));
  ASSERT_TRUE(QMetaObject::invokeMethod(menu, "close"));
  ProcessEvents(40);

  // The mount root and a mounted folder below it.
  h.SetExpanded(2, true);
  for (const uint element : {2u, 3u}) {
    h.fake.SelectFolder(h.fake.UiOf(element));
    EXPECT_TRUE(WaitUntil([&] {
      return !images->property("enabled").toBool() && folder->property("enabled").toBool();
    })) << element;
  }

  h.fake.SelectFolder(0);
  EXPECT_TRUE(WaitUntil([&] { return images->property("enabled").toBool(); }));
  h.panel->setProperty("backendInteractive", false);
  EXPECT_TRUE(WaitUntil([&] {
    return !images->property("enabled").toBool() && !folder->property("enabled").toBool();
  }));
  EXPECT_FALSE(button->isEnabled());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: Import Folder... asks for a folder, then opens the Import Folder dialog with
// Import into Collection selected and the selected collection as the destination. Confirming
// imports into that collection and writes no choice.
TEST(CollectionsPanelQmlTest, ImportFolderOpensTheDialogWithImportSelected) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto* folder_dialog = UseQuickFolderDialog(h);

  ChooseImportFolder(h);
  EXPECT_EQ(h.window->property("lastImportSignal").toString(), QStringLiteral("folder:"));
  AcceptFolderDialog(folder_dialog, dir.path());

  auto* dialog = FlowDialog(h);
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("opened").toBool(); }));
  EXPECT_EQ(dialog->property("selectedAction").toString(), QStringLiteral("import"));
  const QString url = QUrl::fromLocalFile(dir.path()).toString();
  EXPECT_TRUE(h.scan.calls.contains(QStringLiteral("Start:") + url))
      << h.scan.calls.join(',').toStdString();
  auto* import_card = DialogItem(h, QStringLiteral("importFolderDialogCardImport"));
  auto* mount_card  = DialogItem(h, QStringLiteral("importFolderDialogCardMount"));
  ASSERT_NE(import_card, nullptr);
  ASSERT_NE(mount_card, nullptr);
  EXPECT_TRUE(import_card->property("selected").toBool());
  EXPECT_FALSE(mount_card->property("selected").toBool());
  auto* picker = qvariant_cast<QObject*>(dialog->property("destination"));
  ASSERT_NE(picker, nullptr);
  EXPECT_EQ(picker->property("selectedElementId").toInt(), 4);
  auto* confirm = DialogItem(h, QStringLiteral("importFolderDialogConfirm"));
  ASSERT_NE(confirm, nullptr);
  EXPECT_TRUE(WaitUntil([&] { return confirm->isEnabled(); }));
  EXPECT_EQ(confirm->property("text").toString(), QStringLiteral("Import 3 photos"));

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "confirm"));
  ProcessEvents(40);
  EXPECT_FALSE(dialog->property("opened").toBool());
  EXPECT_TRUE(h.import_export.calls.contains(
      QStringLiteral("StartFolderImportToFolderAsUser:%1").arg(h.fake.UiOf(4))))
      << h.import_export.calls.join(',').toStdString();
  EXPECT_EQ(h.behavior.ImportFolderAction(), QStringLiteral("ask"));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: the + of the Disk section opens the dialog with Mount Folder selected, also while
// a mounted row is selected. Remember my choice writes the choice.
TEST(CollectionsPanelQmlTest, DiskPlusOpensTheDialogWithMountSelected) {
  Harness h(MountTree(), /*current_element=*/2);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto* folder_dialog = UseQuickFolderDialog(h);

  ClickDiskAdd(h);
  EXPECT_EQ(h.window->property("lastImportSignal").toString(), QStringLiteral("folder:mount"));
  AcceptFolderDialog(folder_dialog, dir.path());
  auto* dialog = FlowDialog(h);
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("opened").toBool(); }));
  EXPECT_EQ(dialog->property("selectedAction").toString(), QStringLiteral("mount"));
  auto* mount_card  = DialogItem(h, QStringLiteral("importFolderDialogCardMount"));
  auto* destination = DialogItem(h, QStringLiteral("importFolderDialogMountDestination"));
  ASSERT_NE(mount_card, nullptr);
  ASSERT_NE(destination, nullptr);
  EXPECT_TRUE(mount_card->property("selected").toBool());
  EXPECT_TRUE(destination->isVisible());
  auto* confirm = DialogItem(h, QStringLiteral("importFolderDialogConfirm"));
  ASSERT_NE(confirm, nullptr);
  EXPECT_EQ(confirm->property("text").toString(), QStringLiteral("Mount 3 photos"));

  dialog->setProperty("remember", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "confirm"));
  ProcessEvents(60);
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("MountFolder:0:%1").arg(dir.path())))
      << h.fake.calls.join(',').toStdString();
  EXPECT_EQ(h.behavior.ImportFolderAction(), QStringLiteral("mount"));
  // The new mount is a row of the Disk section, below the library root.
  const QVariantMap mount = PanelRow(h, 500);
  ASSERT_FALSE(mount.isEmpty());
  EXPECT_TRUE(mount.value("mountRoot").toBool());
  EXPECT_EQ(mount.value("parentElementId").toUInt(), 0u);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: with a remembered choice, Import Folder... runs it after the system folder
// dialog without the Import Folder dialog. The Disk + names its own action.
TEST(CollectionsPanelQmlTest, RememberedChoiceSkipsTheDialog) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto* folder_dialog = UseQuickFolderDialog(h);
  auto* dialog        = FlowDialog(h);
  ASSERT_NE(dialog, nullptr);
  h.behavior.setImportFolderAction(QStringLiteral("import"));

  ChooseImportFolder(h);
  AcceptFolderDialog(folder_dialog, dir.path());
  const QString url = QUrl::fromLocalFile(dir.path()).toString();
  EXPECT_FALSE(dialog->property("opened").toBool());
  EXPECT_TRUE(h.import_export.calls.contains(
      QStringLiteral("StartFolderImportOfFolderAsUser:%1:%2").arg(url).arg(h.fake.UiOf(4))))
      << h.import_export.calls.join(',').toStdString();

  ClickDiskAdd(h);
  AcceptFolderDialog(folder_dialog, dir.path());
  EXPECT_FALSE(dialog->property("opened").toBool());
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("MountFolder:0:%1").arg(dir.path())))
      << h.fake.calls.join(',').toStdString();
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: the Disk section lists the mounts below a header row per volume, after the
// Collections section. Volumes sort by name; an offline volume shows Offline.
TEST(CollectionsPanelQmlTest, DiskSectionGroupsMountsByVolume) {
  Harness h({{1, 0, QStringLiteral("Trips")},
             {2, 0, QStringLiteral("Archive"), 1, true, false, 0, "vol-d", "Data", "D:"},
             {3, 2, QStringLiteral("Day1"), 1, false, false, 0, "vol-d", "Data", "D:"},
             {4, 0, QStringLiteral("Card"), 2, true, true, 0, "vol-f", "SD", "F:"},
             {5, 0, QStringLiteral("Edits"), 3, true, false, 0, "vol-d", "Data", "D:"}});
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  const auto item = [&h](const QString& name) { return FindItem(h.window->contentItem(), name); };
  auto*      disk_header = item(QStringLiteral("collectionsSection_disk"));
  auto*      data        = item(QStringLiteral("collectionsVolumeRow_vol-d"));
  auto*      sd          = item(QStringLiteral("collectionsVolumeRow_vol-f"));
  ASSERT_NE(disk_header, nullptr);
  ASSERT_NE(data, nullptr);
  ASSERT_NE(sd, nullptr);
  ASSERT_TRUE(h.RowShown(0));
  ASSERT_TRUE(h.RowShown(2));
  ASSERT_TRUE(h.RowShown(5));
  ASSERT_TRUE(h.RowShown(4));
  EXPECT_FALSE(h.RowShown(3));

  // Collections: Library, then Trips; Disk: Data with Archive and Edits, then SD with Card.
  const auto y = [](QQuickItem* row) { return row->y(); };
  EXPECT_LT(y(h.Row(0)), y(h.Row(1)));
  EXPECT_LT(y(h.Row(1)), y(disk_header));
  EXPECT_LT(y(disk_header), y(data));
  EXPECT_LT(y(data), y(h.Row(2)));
  EXPECT_LT(y(h.Row(2)), y(h.Row(5)));
  EXPECT_LT(y(h.Row(5)), y(sd));
  EXPECT_LT(y(sd), y(h.Row(4)));
  EXPECT_EQ(PanelRow(h, 2).value("treeDepth").toInt(), 1);

  EXPECT_EQ(item(QStringLiteral("collectionsVolumeName_vol-d"))->property("text").toString(),
            QStringLiteral("Data"));
  EXPECT_EQ(item(QStringLiteral("collectionsVolumeTag_vol-d"))->property("text").toString(),
            QStringLiteral("D:"));
  EXPECT_EQ(item(QStringLiteral("collectionsVolumeTag_vol-f"))->property("text").toString(),
            QStringLiteral("Offline"));

  // The Disk header folds the Disk section only.
  auto* toggle = item(QStringLiteral("collectionsDiskSectionToggle"));
  ASSERT_NE(toggle, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(toggle, "toggle"));
  ProcessEvents(40);
  EXPECT_FALSE(h.RowShown(2));
  EXPECT_TRUE(h.RowShown(1));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: All Photos selects every photo (SelectFolder 0); the Library row of the tree
// selects the photos directly in the library root (SelectLibraryRoot). Up from the first
// collection selects the Library row.
TEST(CollectionsPanelQmlTest, AllPhotosAndTheLibraryRowSelectTwoScopes) {
  Harness h(SmallTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  auto* all_photos = h.AllPhotos();
  ASSERT_NE(all_photos, nullptr);
  EXPECT_TRUE(all_photos->isVisible());
  EXPECT_EQ(all_photos->property("label").toString(), QStringLiteral("All Photos"));
  ASSERT_TRUE(h.RowShown(0));
  auto* library_icon = FindItem(h.window->contentItem(), QStringLiteral("collectionRowIcon_0"));
  ASSERT_NE(library_icon, nullptr);
  EXPECT_EQ(library_icon->property("source").toUrl().toString(),
            QStringLiteral("qrc:/panel_icons/library.svg"));
  // The Library row starts expanded.
  EXPECT_TRUE(h.RowShown(1));

  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(all_photos));
  EXPECT_TRUE(WaitUntil([&] { return h.fake.calls.contains(QStringLiteral("SelectFolder:0")); }));
  EXPECT_FALSE(h.fake.LibraryRootSelected());
  EXPECT_TRUE(all_photos->property("selected").toBool());
  EXPECT_FALSE(h.Row(0)->property("selected").toBool());

  QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, SceneCenter(h.Row(0)));
  EXPECT_TRUE(WaitUntil([&] { return h.fake.LibraryRootSelected(); }));
  EXPECT_TRUE(h.Row(0)->property("selected").toBool());
  EXPECT_FALSE(all_photos->property("selected").toBool());

  h.fake.SelectFolder(h.fake.UiOf(1));
  ProcessEvents(40);
  h.FocusTree();
  h.Key(Qt::Key_Up);
  EXPECT_TRUE(h.fake.LibraryRootSelected());
  EXPECT_EQ(h.fake.CurrentFolderId(), 0u);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
}

// Issue #1078: the row menu of the Library row has New collection and no Mount Folder Here.
TEST(CollectionsPanelQmlTest, LibraryRowMenuHasNoMountFolderHere) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr);
  auto* menu = h.window->findChild<QObject*>(QStringLiteral("collectionRowMenu"));
  ASSERT_NE(menu, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(h.panel, "openRowMenu", Q_ARG(QVariant, QVariant(PanelRow(h, 0)))));
  ASSERT_TRUE(WaitUntil([&] { return menu->property("visible").toBool(); }));
  EXPECT_EQ(MenuItem(menu, "collectionMenuMountFolder"), nullptr);
  auto* new_child = MenuItem(menu, "collectionMenuNewChild");
  ASSERT_NE(new_child, nullptr);
  EXPECT_EQ(new_child->property("text").toString(), QStringLiteral("New collection"));
}

// Issue #1078: a volume that arrives with no mount shows a card at the top of the Disk section.
// Import... opens the Import Folder dialog for DCIM; on a volume without DCIM, Mount... opens
// the system folder dialog at the volume root. Each button closes the card.
TEST(CollectionsPanelQmlTest, VolumeCardOpensTheImportFolderSteps) {
  Harness h(SmallTree());
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto*      folder_dialog = UseQuickFolderDialog(h);
  const auto card          = [&h] {
    return FindItem(h.window->contentItem(), QStringLiteral("collectionsVolumeCard"));
  };
  EXPECT_EQ(card(), nullptr);

  h.fake.SetArrivedVolume(QStringLiteral("SanDisk"), dir.path(), true);
  ASSERT_TRUE(WaitUntil([&] { return card() != nullptr && card()->height() > 0.0; }));
  auto* disk_header = FindItem(h.window->contentItem(), QStringLiteral("collectionsSection_disk"));
  ASSERT_NE(disk_header, nullptr);
  EXPECT_GT(card()->y(), disk_header->y());
  EXPECT_EQ(
      FindItem(card(), QStringLiteral("collectionsVolumeCardName"))->property("text").toString(),
      QStringLiteral("SanDisk"));
  EXPECT_TRUE(FindItem(card(), QStringLiteral("collectionsVolumeCardDcim"))->isVisible());

  ASSERT_TRUE(QMetaObject::invokeMethod(
      FindItem(card(), QStringLiteral("collectionsVolumeCardImport")), "clicked"));
  ProcessEvents(60);
  const QString dcim = QUrl::fromLocalFile(dir.path() + QStringLiteral("/DCIM")).toString();
  EXPECT_EQ(h.window->property("lastImportSignal").toString(), QStringLiteral("folder:import"));
  EXPECT_EQ(h.window->property("lastImportFolderUrl").toString(), dcim);
  EXPECT_TRUE(h.fake.calls.contains(QStringLiteral("TakeArrivedVolume")));
  EXPECT_TRUE(WaitUntil([&] { return card() == nullptr; }));
  auto* dialog = FlowDialog(h);
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("opened").toBool(); }));
  EXPECT_EQ(dialog->property("selectedAction").toString(), QStringLiteral("import"));
  EXPECT_TRUE(h.scan.calls.contains(QStringLiteral("Start:") + dcim));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
  ProcessEvents(60);
  EXPECT_TRUE(h.scan.calls.contains(QStringLiteral("Cancel")));

  h.fake.SetArrivedVolume(QStringLiteral("Disk"), dir.path(), false);
  ASSERT_TRUE(WaitUntil([&] { return card() != nullptr && card()->height() > 0.0; }));
  EXPECT_FALSE(FindItem(card(), QStringLiteral("collectionsVolumeCardDcim"))->isVisible());
  ASSERT_TRUE(QMetaObject::invokeMethod(
      FindItem(card(), QStringLiteral("collectionsVolumeCardMount")), "clicked"));
  ASSERT_TRUE(WaitUntil([&] { return folder_dialog->property("visible").toBool(); }));
  EXPECT_EQ(h.window->property("lastImportSignal").toString(), QStringLiteral("folder:mount"));
  EXPECT_EQ(h.window->property("lastImportStartUrl").toString(),
            QUrl::fromLocalFile(dir.path()).toString());
  ASSERT_TRUE(QMetaObject::invokeMethod(folder_dialog, "reject"));
  ProcessEvents(40);

  // #1087: the volume of the card is gone or was replaced; the button closes the card and opens
  // no step.
  h.window->setProperty("lastImportSignal", QString());
  h.fake.arrived_volume_present = false;
  h.fake.SetArrivedVolume(QStringLiteral("SanDisk"), dir.path(), true);
  ASSERT_TRUE(WaitUntil([&] { return card() != nullptr && card()->height() > 0.0; }));
  ASSERT_TRUE(QMetaObject::invokeMethod(
      FindItem(card(), QStringLiteral("collectionsVolumeCardImport")), "clicked"));
  ProcessEvents(60);
  EXPECT_TRUE(WaitUntil([&] { return card() == nullptr; }));
  EXPECT_TRUE(h.window->property("lastImportSignal").toString().isEmpty());
  EXPECT_FALSE(dialog->property("opened").toBool());
}

// Issue #1078: the destination tree lists Library and the ordinary collections. A search keeps
// each match with its ancestors and expands every branch; clearing it restores the expansion of
// the tree. The matched part is bold, the destination stays selected while it is hidden, and no
// match shows one muted row.
TEST(CollectionsPanelQmlTest, DestinationTreeSearchKeepsAncestorsAndRestoresTheExpansion) {
  Harness h(PickerTree(), /*current_element=*/4);
  ASSERT_NE(h.panel, nullptr) << h.warnings.join('\n').toStdString();
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto* flow = Flow(h);
  ASSERT_NE(flow, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(flow, "begin", Q_ARG(QVariant, QString()),
                                        Q_ARG(QVariant, QUrl::fromLocalFile(dir.path()).toString()),
                                        Q_ARG(QVariant, QString())));
  auto* dialog = FlowDialog(h);
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return dialog->property("opened").toBool(); }));
  auto* picker = qvariant_cast<QObject*>(dialog->property("destination"));
  ASSERT_NE(picker, nullptr);

  // Sorted by name; Trips is collapsed; the mount Archive is not a destination.
  EXPECT_EQ(PickerElements(picker), (std::vector<uint>{0, 4, 1}));
  EXPECT_EQ(picker->property("selectedElementId").toInt(), 4);
  QVariant trips;
  for (const auto& value : picker->property("shownRows").toList()) {
    if (value.toMap().value("elementId").toUInt() == 1u) {
      trips = value;
    }
  }
  ASSERT_TRUE(trips.isValid());
  ASSERT_TRUE(QMetaObject::invokeMethod(picker, "setExpanded", Q_ARG(QVariant, trips),
                                        Q_ARG(QVariant, true)));
  ProcessEvents(30);
  EXPECT_EQ(PickerElements(picker), (std::vector<uint>{0, 4, 1, 2}));

  // A match below Kyoto: its ancestors stay, every branch opens.
  picker->setProperty("searchText", QStringLiteral("da"));
  ProcessEvents(30);
  EXPECT_EQ(PickerElements(picker), (std::vector<uint>{0, 1, 2, 3}));
  auto* day_name = DialogItem(h, QStringLiteral("collectionPickerName_3"));
  ASSERT_NE(day_name, nullptr);
  EXPECT_EQ(day_name->property("text").toString(), QStringLiteral("<b>Da</b>y"));
  // Portraits is hidden and stays the destination.
  EXPECT_EQ(picker->property("selectedElementId").toInt(), 4);
  EXPECT_EQ(picker->property("selectedFolderId").toUInt(), h.fake.UiOf(4));

  picker->setProperty("searchText", QStringLiteral("zzz"));
  ProcessEvents(30);
  EXPECT_TRUE(PickerElements(picker).empty());
  auto* empty = DialogItem(h, QStringLiteral("collectionPickerEmpty"));
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(empty->isVisible());
  EXPECT_EQ(empty->property("text").toString(), QStringLiteral("No matching collections"));

  picker->setProperty("searchText", QString());
  ProcessEvents(30);
  EXPECT_EQ(PickerElements(picker), (std::vector<uint>{0, 4, 1, 2}));
  EXPECT_FALSE(empty->isVisible());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.warnings.join('\n').toStdString();
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
