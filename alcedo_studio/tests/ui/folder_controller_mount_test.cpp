//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF6 (#988) and MF8 (#990): the mount fields of the folder rows and the
// mount commands of FolderController, in an ApplicationModuleHost with a real project, a real
// scan coordinator, and real directories.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

#include "app/mount_service.hpp"
#include "app/project_service.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"

namespace alcedo::ui::test {
namespace {

namespace stdfs = std::filesystem;

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

class FolderControllerMountTest : public ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    disk_ = temp_dir_ / "disk";
    WriteJpeg(disk_ / "Trip" / "a.jpg", 1);
    WriteJpeg(disk_ / "Trip" / "Day1" / "b.jpg", 2);
    disk_ = stdfs::canonical(disk_);
  }

  auto Folders() -> FolderController& { return *backend_.folders(); }
  auto Mounts() -> MountService& {
    return *backend_.project()->handler().project()->GetMountService();
  }

  /// The row whose name is @p name; empty when no row has it.
  auto RowNamed(const QString& name) -> std::optional<QVariantMap> {
    for (const auto& value : Folders().Folders()) {
      const auto row = value.toMap();
      if (row.value("name").toString() == name) {
        return row;
      }
    }
    return std::nullopt;
  }

  /// The photo count of the library root row: the whole project.
  auto ProjectPhotoCount() -> qint64 {
    return Folders().Folders().front().toMap().value("imageCount").toLongLong();
  }

  auto Summary() -> MountSummary {
    const auto summaries = Mounts().ListMounts();
    EXPECT_EQ(summaries.size(), 1u);
    return summaries.empty() ? MountSummary{} : summaries.front();
  }

  /// Mount `disk_/Trip` in the root row and wait for the first scan and the row counts.
  void MountTrip() {
    ASSERT_TRUE(CreateTestProject(backend_));
    ASSERT_TRUE(backend_.mount_scans()->IsStarted());
    Folders().MountFolder(0, QUrl::fromLocalFile(PathToQString(disk_ / "Trip")));
    // A row counts the photos of its own folder: the root row counts the project.
    ASSERT_TRUE(WaitFor([this] {
      return RowNamed("Trip").has_value() && RowNamed("Day1").has_value() &&
             ProjectPhotoCount() == 2 && !backend_.mount_scans()->HasPendingScans();
    }));
  }

  /// Request a Synchronize scan and wait for its end and the next row read.
  void Synchronize() {
    const auto row = RowNamed("Trip");
    ASSERT_TRUE(row.has_value());
    bool ended = false;
    ASSERT_TRUE(Folders()
                    .StartSynchronize(row->value("mountId").toUInt(),
                                      [&ended](const MountScanResult&) { ended = true; })
                    .isEmpty());
    ASSERT_TRUE(WaitFor([&ended] { return ended; }));
    // The scan end starts a count read, which carries the mount states.
    QCoreApplication::processEvents();
    ProcessEvents(300);
  }

  ApplicationModuleHost backend_;
  stdfs::path           disk_;
};

TEST_F(FolderControllerMountTest, MountedRowReportsMountedAndNotStructureEditable) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());

  const auto root   = RowNamed("Trip");
  const auto folder = RowNamed("Day1");
  ASSERT_TRUE(root.has_value());
  ASSERT_TRUE(folder.has_value());
  EXPECT_GT(root->value("mountId").toUInt(), 0u);
  EXPECT_TRUE(root->value("mountRoot").toBool());
  EXPECT_FALSE(root->value("structureEditable").toBool());
  EXPECT_FALSE(root->value("mountOffline").toBool());
  EXPECT_EQ(root->value("mountMissingCount").toULongLong(), 0u);
  EXPECT_EQ(folder->value("mountId").toUInt(), root->value("mountId").toUInt());
  EXPECT_FALSE(folder->value("mountRoot").toBool());
  EXPECT_FALSE(folder->value("structureEditable").toBool());

  // The library root row stays editable and is not mounted.
  const auto library_root = Folders().Folders().front().toMap();
  EXPECT_EQ(library_root.value("mountId").toUInt(), 0u);
  EXPECT_TRUE(library_root.value("structureEditable").toBool());

  EXPECT_FALSE(Folders().CurrentFolderMounted());
  Folders().SelectFolder(folder->value("folderId").toUInt());
  EXPECT_TRUE(Folders().CurrentFolderMounted());

  // Each mounted photo is present and stays in the library.
  QVariantList ids;
  for (const auto id : Summary().present_element_ids) {
    ids.push_back(static_cast<qulonglong>(id));
  }
  EXPECT_EQ(ids.size(), 2);
  EXPECT_TRUE(Folders().HoldsPresentMountedPhoto(ids));
}

TEST_F(FolderControllerMountTest, OfflineMountRowReportsOfflineUntilLocateFolder) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  const auto ids = Summary().present_element_ids;

  stdfs::rename(disk_ / "Trip", disk_ / "Trip 2026");
  ASSERT_NO_FATAL_FAILURE(Synchronize());
  ASSERT_TRUE(WaitFor([this] { return RowNamed("Trip")->value("mountOffline").toBool(); }));
  EXPECT_EQ(RowNamed("Trip")->value("mountMissingCount").toULongLong(), 0u);
  EXPECT_TRUE(RowNamed("Day1")->value("mountOffline").toBool());

  Folders().LocateMountFolder(RowNamed("Trip")->value("folderId").toUInt(),
                              QUrl::fromLocalFile(PathToQString(disk_ / "Trip 2026")));
  ASSERT_TRUE(WaitFor([this] {
    return !RowNamed("Trip")->value("mountOffline").toBool() &&
           !backend_.mount_scans()->HasPendingScans();
  }));
  EXPECT_EQ(Summary().record.root_path, disk_ / "Trip 2026");
  EXPECT_EQ(Summary().present_element_ids, ids);
  EXPECT_EQ(Summary().missing_count, 0u);
}

TEST_F(FolderControllerMountTest, RemoveMissingPhotosRemovesTheMissingPhotoOfTheRow) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  stdfs::remove(disk_ / "Trip" / "a.jpg");
  ASSERT_NO_FATAL_FAILURE(Synchronize());
  ASSERT_TRUE(
      WaitFor([this] { return RowNamed("Trip")->value("mountMissingCount").toULongLong() == 1; }));
  const auto missing_id = Summary().missing_element_ids.front();
  EXPECT_TRUE(Folders().IsMissingMountedPhoto(static_cast<uint>(missing_id)));
  EXPECT_TRUE(backend_.library()->thumbs().IsStoredMissingSource(missing_id));

  Folders().RemoveMissingPhotos(RowNamed("Trip")->value("folderId").toUInt());
  ASSERT_TRUE(WaitFor([this] {
    return RowNamed("Trip")->value("mountMissingCount").toULongLong() == 0 &&
           ProjectPhotoCount() == 1;
  }));
  EXPECT_FALSE(Folders().IsMissingMountedPhoto(static_cast<uint>(missing_id)));
  EXPECT_FALSE(backend_.library()->thumbs().IsStoredMissingSource(missing_id));
  EXPECT_EQ(Summary().present_count, 1u);
  EXPECT_EQ(RowNamed("Day1")->value("imageCount").toLongLong(), 1);
}

TEST_F(FolderControllerMountTest, LocatedPhotoIsPresentAgain) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  stdfs::remove(disk_ / "Trip" / "Day1" / "b.jpg");
  ASSERT_NO_FATAL_FAILURE(Synchronize());
  ASSERT_TRUE(
      WaitFor([this] { return RowNamed("Trip")->value("mountMissingCount").toULongLong() == 1; }));
  const auto        missing_id    = Summary().missing_element_ids.front();
  // The dialog opens in the last directory of the photo.
  const stdfs::path locate_folder = Folders()
                                        .MissingPhotoLocateFolder(static_cast<uint>(missing_id))
                                        .toLocalFile()
                                        .toStdWString();
  EXPECT_EQ(locate_folder.lexically_normal(), (disk_ / "Trip" / "Day1").lexically_normal());

  // The rewritten file has another fingerprint, so only Locate joins it to the photo.
  WriteJpeg(disk_ / "Trip" / "Day1" / "b-edited.jpg", 9);
  Folders().LocateMissingPhoto(
      static_cast<uint>(missing_id),
      QUrl::fromLocalFile(PathToQString(disk_ / "Trip" / "Day1" / "b-edited.jpg")));
  ASSERT_TRUE(
      WaitFor([this] { return RowNamed("Trip")->value("mountMissingCount").toULongLong() == 0; }));
  EXPECT_FALSE(Folders().IsMissingMountedPhoto(static_cast<uint>(missing_id)));
  const auto present = Summary().present_element_ids;
  EXPECT_TRUE(std::find(present.begin(), present.end(), missing_id) != present.end());
  EXPECT_EQ(ProjectPhotoCount(), 2);
  EXPECT_EQ(RowNamed("Day1")->value("imageCount").toLongLong(), 1);
}

TEST_F(FolderControllerMountTest, UnmountLeavesOrdinaryRowsAndPhotos) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  Folders().Unmount(RowNamed("Trip")->value("folderId").toUInt());
  ASSERT_TRUE(WaitFor([this] { return !RowNamed("Trip").has_value(); }));
  EXPECT_TRUE(Mounts().ListMounts().empty());
  ASSERT_TRUE(WaitFor([this] { return ProjectPhotoCount() == 2; }));
}

TEST_F(FolderControllerMountTest, MountInsideAMountedFolderIsRefusedWithNoRow) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  const auto rows_before = Folders().Folders().size();
  Folders().MountFolder(0, QUrl::fromLocalFile(PathToQString(disk_ / "Trip" / "Day1")));
  ASSERT_TRUE(WaitFor([this] { return !backend_.mount_scans()->HasPendingScans(); }));
  ProcessEvents(300);
  EXPECT_EQ(Folders().Folders().size(), rows_before);
  EXPECT_EQ(Mounts().ListMounts().size(), 1u);
}

}  // namespace
}  // namespace alcedo::ui::test
