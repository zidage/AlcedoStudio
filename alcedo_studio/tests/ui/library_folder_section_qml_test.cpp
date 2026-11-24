//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file library_folder_section_qml_test.cpp
/// @brief The folder section of the Library view (issue #1009) in the production Main.qml.
///
/// Main.qml loads from the source tree against an ApplicationModuleHost with a real project,
/// a real scan coordinator, and real directories. Two mounts give the folder trees:
///
///   Trip/  HDR/h1.jpg  JPEG/j1.jpg  RAW/r1.jpg r2.jpg           (no own photo)
///   Day/   d1.jpg d2.jpg  Notes/n1.jpg  Sub/s1.jpg               (own photos)
///
/// A scan creates a folder only for a directory with a photo at some depth. A mounted folder
/// without photos is the state after Remove Missing Photos and before the next scan.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <QuickQanava>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include "app/mount_service.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace alcedo::ui::test {
namespace {

namespace stdfs = std::filesystem;

auto MainQmlUrl() -> QUrl {
  const auto path = stdfs::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" / "Main.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

/// Encode a 24x16 JPEG whose pixels depend on @p seed, so two seeds give two fingerprints.
void WriteJpeg(const stdfs::path& path, int seed) {
  stdfs::create_directories(path.parent_path());
  constexpr int             kWidth = 24, kHeight = 16, kChannels = 3;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * kChannels);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    pixels[index] = static_cast<std::uint8_t>(index * 7 + seed * 31);
  }
  auto output = OIIO::ImageOutput::create(path.string());
  ASSERT_TRUE(output);
  const OIIO::ImageSpec spec(kWidth, kHeight, kChannels, OIIO::TypeDesc::UINT8);
  ASSERT_TRUE(output->open(path.string(), spec));
  ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
  ASSERT_TRUE(output->close());
}

auto WaitFor(const std::function<bool()>& condition, int timeout_ms = 20000) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (condition()) {
      return true;
    }
    QThread::msleep(10);
  }
  return condition();
}

/// Items in the visual tree under @p root whose object name starts with @p prefix. Delegates
/// have no QObject parent, so QObject::findChildren does not see them.
void CollectItems(QQuickItem* root, const QString& prefix, std::vector<QQuickItem*>& out) {
  if (root == nullptr) {
    return;
  }
  if (root->objectName().startsWith(prefix)) {
    out.push_back(root);
  }
  for (auto* child : root->childItems()) {
    CollectItems(child, prefix, out);
  }
}

auto FindItem(QQuickItem* root, const QString& name) -> QQuickItem* {
  std::vector<QQuickItem*> found;
  CollectItems(root, name, found);
  for (auto* item : found) {
    if (item->objectName() == name) {
      return item;
    }
  }
  return nullptr;
}

/// True when @p item and each of its ancestors is visible and has a size.
auto ShownOnScreen(QQuickItem* item) -> bool {
  return item != nullptr && item->isVisible() && item->width() > 0 && item->height() > 0;
}

/// One shown folder tile: its name and its count text.
struct ShownTile {
  QQuickItem* item = nullptr;
  QString     name;
  QString     count;
  bool        link = false;
};

class LibraryFolderSectionQmlTest : public ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    disk_ = temp_dir_ / "disk";
    WriteJpeg(disk_ / "Trip" / "HDR" / "h1.jpg", 1);
    WriteJpeg(disk_ / "Trip" / "JPEG" / "j1.jpg", 2);
    WriteJpeg(disk_ / "Trip" / "RAW" / "r1.jpg", 3);
    WriteJpeg(disk_ / "Trip" / "RAW" / "r2.jpg", 4);
    WriteJpeg(disk_ / "Day" / "d1.jpg", 5);
    WriteJpeg(disk_ / "Day" / "d2.jpg", 6);
    WriteJpeg(disk_ / "Day" / "Sub" / "s1.jpg", 7);
    WriteJpeg(disk_ / "Day" / "Notes" / "n1.jpg", 8);
    disk_ = stdfs::canonical(disk_);
  }

  void TearDown() override {
    if (backend_.library_view() != nullptr) {
      backend_.library_view()->setGridLayout(QStringLiteral("classic"));
    }
    engine_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  auto Folders() -> FolderController& { return *backend_.folders(); }

  auto RowNamed(const QString& name) -> std::optional<QVariantMap> {
    for (const auto& value : Folders().Folders()) {
      const auto row = value.toMap();
      if (row.value("name").toString() == name) {
        return row;
      }
    }
    return std::nullopt;
  }

  auto UiIdOf(const QString& name) -> uint {
    const auto row = RowNamed(name);
    return row.has_value() ? row->value("folderId").toUInt() : 0u;
  }

  /// Create the project, mount Trip and Day, and wait for the scans and the row counts.
  void MountBoth() {
    ASSERT_TRUE(CreateTestProject(backend_));
    ASSERT_TRUE(backend_.mount_scans()->IsStarted());
    Folders().MountFolder(QUrl::fromLocalFile(PathToQString(disk_ / "Trip")));
    ASSERT_TRUE(WaitFor([this] {
      return RowNamed("RAW").has_value() && !backend_.mount_scans()->HasPendingScans();
    }));
    Folders().MountFolder(QUrl::fromLocalFile(PathToQString(disk_ / "Day")));
    ASSERT_TRUE(WaitFor([this] {
      const auto trip = RowNamed("Trip");
      return RowNamed("Sub").has_value() && trip.has_value() && RowNamed("Notes").has_value() &&
             trip->value("imageCount").toLongLong() == 4 &&
             !backend_.mount_scans()->HasPendingScans();
    }));
  }

  /// Load the production Main.qml and wait for the Library workspace.
  void LoadMain() {
    AppTheme::RegisterFonts();
    language_manager_ = std::make_unique<LanguageManager>(QCoreApplication::instance());
    AppTheme::SetEffectiveLanguageCode(QStringLiteral("en"));
    QQuickStyle::setStyle(QStringLiteral("Material"));
    AppTheme::Instance().setReduceMotion(true);

    engine_ = std::make_unique<QQmlApplicationEngine>();
    engine_->addImportPath(QStringLiteral("qrc:/"));
    QuickQanava::initialize(engine_.get());
    language_manager_->AttachEngine(engine_.get());
    backend_.AttachQmlEngine(engine_.get());
    engine_->rootContext()->setContextProperty(QStringLiteral("appModules"), &backend_);
    engine_->rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_->rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                               language_manager_.get());
    QObject::connect(engine_.get(), &QQmlEngine::warnings,
                     [this](const QList<QQmlError>& warnings) {
                       for (const auto& warning : warnings) {
                         warnings_.push_back(warning.toString().toStdString());
                       }
                     });
    engine_->load(MainQmlUrl());
    ASSERT_FALSE(engine_->rootObjects().empty()) << Warnings();
    window_ = qobject_cast<QQuickWindow*>(engine_->rootObjects().front());
    ASSERT_NE(window_, nullptr);
    window_->resize(1400, 900);
    backend_.workspace_router()->OpenLibrary();
    ASSERT_TRUE(WaitFor([this] { return Workspace() != nullptr; })) << Warnings();
    ProcessEvents(200);
  }

  auto Warnings() const -> std::string {
    std::ostringstream out;
    for (const auto& warning : warnings_) {
      out << warning << '\n';
    }
    return out.str();
  }

  /// Warnings that name the files of this feature.
  auto FeatureWarnings() const -> std::string {
    std::ostringstream out;
    for (const auto& warning : warnings_) {
      if (warning.find("LibraryFolderSection") != std::string::npos ||
          warning.find("folderSection") != std::string::npos) {
        out << warning << '\n';
      }
    }
    return out.str();
  }

  auto Workspace() -> QQuickItem* {
    return window_ != nullptr ? FindItem(window_->contentItem(), QStringLiteral("libraryWorkspace"))
                              : nullptr;
  }

  /// The shown tiles of the folder section, top to bottom and left to right.
  auto ShownTiles() -> std::vector<ShownTile> {
    std::vector<QQuickItem*> items;
    CollectItems(Workspace(), QStringLiteral("libraryFolderTile_"), items);
    std::vector<ShownTile> out;
    for (auto* item : items) {
      // A tile counts when its section is shown in the view that is shown.
      bool shown = true;
      for (auto* parent = item; parent != nullptr; parent = parent->parentItem()) {
        if (!parent->isVisible()) {
          shown = false;
          break;
        }
      }
      if (!shown || item->width() <= 0) {
        continue;
      }
      auto* name  = FindItem(item, QStringLiteral("libraryFolderTileName"));
      auto* count = FindItem(item, QStringLiteral("libraryFolderTileCount"));
      auto* link  = FindItem(item, QStringLiteral("libraryFolderTileLink"));
      out.push_back({item, name ? name->property("text").toString() : QString{},
                     count ? count->property("text").toString() : QString{},
                     link != nullptr && link->isVisible()});
    }
    std::sort(out.begin(), out.end(), [](const ShownTile& a, const ShownTile& b) {
      const auto pa = a.item->mapToScene(QPointF(0, 0));
      const auto pb = b.item->mapToScene(QPointF(0, 0));
      return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    return out;
  }

  auto EmptyStateShown() -> bool {
    auto* empty = FindItem(Workspace(), QStringLiteral("libraryEmptyState"));
    return empty != nullptr && empty->isVisible();
  }

  void Select(const QString& name) {
    const uint id = UiIdOf(name);
    ASSERT_NE(id, 0u) << name.toStdString();
    Folders().SelectFolder(id);
    ProcessEvents(100);
  }

  /// The photo view of the workspace (grid or grouped), once its photos are shown.
  auto WaitForPhotoView(const char* object_name) -> QQuickItem* {
    QQuickItem* view = nullptr;
    WaitFor([&] {
      view = FindItem(Workspace(), QString::fromLatin1(object_name));
      return view != nullptr && backend_.library()->ShownCount() > 0;
    });
    ProcessEvents(200);
    return view;
  }

  /// The tiles of the Day section: Notes then Sub, above the first photo of the view.
  void ExpectDaySectionAbovePhotos(QQuickItem* view) {
    ASSERT_NE(view, nullptr) << Warnings();
    ASSERT_TRUE(WaitFor([this] { return ShownTiles().size() == 2; }));
    const auto tiles = ShownTiles();
    EXPECT_EQ(tiles[0].name, QStringLiteral("Notes"));
    EXPECT_EQ(tiles[0].count, QStringLiteral("1"));
    EXPECT_EQ(tiles[1].name, QStringLiteral("Sub"));
    EXPECT_EQ(tiles[1].count, QStringLiteral("1"));
    EXPECT_TRUE(tiles[0].link);
    EXPECT_FALSE(EmptyStateShown());

    // The section stands at the top of the view, and the photos follow below it.
    auto* section = FindItem(view, QStringLiteral("libraryFolderSection"));
    ASSERT_NE(section, nullptr);
    ASSERT_TRUE(ShownOnScreen(section));
    const double section_top    = section->mapToItem(view, QPointF(0, 0)).y();
    const double section_bottom = section_top + section->height();
    // At the top of the view (the grouped view keeps its Expand all row above the list).
    EXPECT_GE(section_top, -0.5);
    EXPECT_LT(section_top, 60.0);
    EXPECT_GT(section->height(), 0.0);
    std::vector<QQuickItem*> items;
    CollectItems(view, QStringLiteral(""), items);
    double first_photo_top = 1e9;
    for (auto* item : items) {
      // Classic card (cardIndex), justified tile (photoIndex), or grouped cell (fileId).
      const bool classic =
          item->property("cardIndex").isValid() && item->property("elementId").toInt() > 0;
      const bool justified =
          item->property("photoIndex").isValid() && item->property("photoIndex").toInt() >= 0;
      const bool grouped = item->objectName() == QStringLiteral("albumSectionPhotoCell") &&
                           item->property("fileId").toInt() > 0;
      if ((classic || justified || grouped) && item->isVisible() && item->width() > 0) {
        first_photo_top = std::min(first_photo_top, item->mapToItem(view, QPointF(0, 0)).y());
      }
    }
    ASSERT_LT(first_photo_top, 1e9) << "no photo card in the view";
    EXPECT_GE(first_photo_top + 0.5, section_bottom);
  }

  ApplicationModuleHost                  backend_;
  stdfs::path                            disk_;
  std::unique_ptr<LanguageManager>       language_manager_;
  std::unique_ptr<QQmlApplicationEngine> engine_;
  QQuickWindow*                          window_ = nullptr;
  std::vector<std::string>               warnings_;
};

}  // namespace

// A folder with three child folders and no own photo shows three tiles in the tree order, with
// the subtree counts and the link icon of a mounted folder, and no empty state.
TEST_F(LibraryFolderSectionQmlTest, ParentWithChildFoldersAndNoPhotoShowsTheTilesAndNoEmptyState) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Trip")));

  ASSERT_TRUE(WaitFor([this] { return ShownTiles().size() == 3; })) << FeatureWarnings();
  const auto tiles = ShownTiles();
  EXPECT_EQ(tiles[0].name, QStringLiteral("HDR"));
  EXPECT_EQ(tiles[1].name, QStringLiteral("JPEG"));
  EXPECT_EQ(tiles[2].name, QStringLiteral("RAW"));
  EXPECT_EQ(tiles[0].count, QStringLiteral("1"));
  EXPECT_EQ(tiles[1].count, QStringLiteral("1"));
  EXPECT_EQ(tiles[2].count, QStringLiteral("2"));
  for (const auto& tile : tiles) {
    EXPECT_TRUE(tile.link) << tile.name.toStdString();
  }
  EXPECT_EQ(backend_.library()->ShownCount(), 0);
  EXPECT_FALSE(EmptyStateShown());
  // The parent counts the photos of its subtree (#1007).
  EXPECT_EQ(RowNamed("Trip")->value("imageCount").toLongLong(), 4);
  EXPECT_TRUE(FeatureWarnings().empty()) << FeatureWarnings();
}

// A click on a folder tile selects that folder.
TEST_F(LibraryFolderSectionQmlTest, ClickOnAFolderTileSelectsThatFolder) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Trip")));
  ASSERT_TRUE(WaitFor([this] { return ShownTiles().size() == 3; })) << FeatureWarnings();

  const auto  tiles = ShownTiles();
  const auto* raw   = &tiles[2];
  const auto  center =
      raw->item->mapToScene(QPointF(raw->item->width() / 2.0, raw->item->height() / 2.0));
  QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center.toPoint());
  ASSERT_TRUE(WaitFor([this] { return Folders().CurrentFolderId() == UiIdOf("RAW"); }));
  // The leaf shows its photos and no folder section.
  ASSERT_TRUE(WaitFor([this] { return backend_.library()->ShownCount() == 2; }));
  ProcessEvents(200);
  EXPECT_TRUE(ShownTiles().empty());
}

// A double click on a folder tile shows that folder; its second click opens no photo of the
// view below the pointer.
TEST_F(LibraryFolderSectionQmlTest, DoubleClickOnAFolderTileOpensNoPhoto) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  backend_.library_view()->setGridLayout(QStringLiteral("classic"));
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Day")));
  ASSERT_NE(WaitForPhotoView("libraryThumbnailGridView"), nullptr);
  ASSERT_TRUE(WaitFor([this] { return ShownTiles().size() == 2; })) << FeatureWarnings();

  const auto tiles  = ShownTiles();
  const auto center = tiles[1].item->mapToScene(
      QPointF(tiles[1].item->width() / 2.0, tiles[1].item->height() / 2.0));
  QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, center.toPoint());
  ASSERT_TRUE(WaitFor([this] { return Folders().CurrentFolderId() == UiIdOf("Sub"); }));
  ProcessEvents(500);
  EXPECT_EQ(backend_.workspace_router()->workspace(), QStringLiteral("library"));
}

// A text search selects photos: the folder section is hidden.
TEST_F(LibraryFolderSectionQmlTest, ActiveTextSearchHidesTheFolderSection) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Day")));
  ASSERT_TRUE(WaitFor([this] { return ShownTiles().size() == 2; })) << FeatureWarnings();

  backend_.search()->ApplyFuzzySearch(QStringLiteral("d1"));
  ASSERT_TRUE(WaitFor([this] { return !backend_.search()->active_search_query().isEmpty(); }));
  ProcessEvents(300);
  EXPECT_TRUE(ShownTiles().empty());
  auto* section = FindItem(Workspace(), QStringLiteral("libraryFolderSection"));
  EXPECT_TRUE(section == nullptr || !section->isVisible() || section->height() == 0.0);
}

// A mounted leaf folder without photos shows the empty state without the import text.
TEST_F(LibraryFolderSectionQmlTest, MountedLeafWithoutPhotosShowsTheEmptyStateWithoutImportText) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  // The photo of Notes leaves the disk; a scan marks it missing, and Remove Missing Photos
  // removes it. Notes stays without photos until the next scan.
  stdfs::remove(disk_ / "Day" / "Notes" / "n1.jpg");
  bool ended = false;
  ASSERT_TRUE(Folders()
                  .StartSynchronize(RowNamed("Day")->value("mountId").toUInt(),
                                    [&ended](const MountScanResult&) { ended = true; })
                  .isEmpty());
  ASSERT_TRUE(WaitFor([&ended] { return ended; }));
  ASSERT_TRUE(WaitFor([this] {
    return RowNamed("Day")->value("mountMissingCount").toInt() == 1 &&
           !backend_.mount_scans()->HasPendingScans();
  }));
  Folders().RemoveMissingPhotos(UiIdOf("Day"));
  ASSERT_TRUE(WaitFor([this] {
    const auto notes = RowNamed("Notes");
    return notes.has_value() && notes->value("imageCount").toLongLong() == 0 &&
           RowNamed("Day")->value("mountMissingCount").toInt() == 0;
  }));
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Notes")));

  ASSERT_TRUE(WaitFor([this] { return EmptyStateShown(); })) << FeatureWarnings();
  EXPECT_TRUE(ShownTiles().empty());
  auto* detail = FindItem(Workspace(), QStringLiteral("libraryEmptyStateDetail"));
  ASSERT_NE(detail, nullptr);
  EXPECT_FALSE(detail->isVisible()) << detail->property("text").toString().toStdString();
}

// Classic grid: the section stands above the first row of photos and scrolls with them.
TEST_F(LibraryFolderSectionQmlTest, ClassicGridShowsTheSectionAboveThePhotos) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  backend_.library_view()->setGridLayout(QStringLiteral("classic"));
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Day")));
  auto* view = WaitForPhotoView("libraryThumbnailGridView");
  ASSERT_NO_FATAL_FAILURE(ExpectDaySectionAbovePhotos(view));
  EXPECT_GT(view->property("topInset").toDouble(), 0.0);
  EXPECT_TRUE(FeatureWarnings().empty()) << FeatureWarnings();
}

// Justified rows: the same section above the first row.
TEST_F(LibraryFolderSectionQmlTest, JustifiedRowsShowTheSectionAboveThePhotos) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  backend_.library_view()->setGridLayout(QStringLiteral("justified"));
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Day")));
  auto* view = WaitForPhotoView("libraryThumbnailGridView");
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(WaitFor([view] { return view->property("justified").toBool(); }));
  ASSERT_NO_FATAL_FAILURE(ExpectDaySectionAbovePhotos(view));
  EXPECT_TRUE(FeatureWarnings().empty()) << FeatureWarnings();
}

// Grouped photos: the section stands above the first group, outside every group.
TEST_F(LibraryFolderSectionQmlTest, GroupedPhotosShowTheSectionAboveTheFirstGroup) {
  ASSERT_NO_FATAL_FAILURE(MountBoth());
  ASSERT_NO_FATAL_FAILURE(LoadMain());
  backend_.library()->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_NO_FATAL_FAILURE(Select(QStringLiteral("Day")));
  auto* view = WaitForPhotoView("libraryAlbumSectionView");
  ASSERT_NO_FATAL_FAILURE(ExpectDaySectionAbovePhotos(view));
  backend_.library()->SetInspectorGrouping(QStringLiteral("camera"), false);
  EXPECT_TRUE(FeatureWarnings().empty()) << FeatureWarnings();
}

}  // namespace alcedo::ui::test
