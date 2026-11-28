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
#include <QSignalSpy>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "app/mount_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "utils/file/volume_identity.hpp"
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

  /// All Photos: every photo of the project. The root row counts the Library row, which leaves
  /// out the mount trees (#1076).
  auto ProjectPhotoCount() -> qint64 { return Folders().AllPhotosCount(); }

  auto Summary() -> MountSummary {
    const auto summaries = Mounts().ListMounts();
    EXPECT_EQ(summaries.size(), 1u);
    return summaries.empty() ? MountSummary{} : summaries.front();
  }

  /// Mount `disk_/Trip` in the root row and wait for the first scan and the row counts.
  void MountTrip() {
    ASSERT_TRUE(CreateTestProject(backend_));
    ASSERT_TRUE(backend_.mount_scans()->IsStarted());
    Folders().MountFolder(QUrl::fromLocalFile(PathToQString(disk_ / "Trip")));
    // A row counts the photos of its own folder; All Photos counts the project.
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

// Issue #1078: the Library row reads the photos directly in the library root and counts the root
// and the ordinary collections, without the mount trees. All Photos reads and counts every photo.
// A mounted row carries the volume of its mount for the Disk section.
TEST_F(FolderControllerMountTest, LibraryRowLeavesOutTheMountedPhotosAndAllPhotosHasThem) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());

  const auto root_row = Folders().Folders().front().toMap();
  EXPECT_EQ(root_row.value("folderId").toUInt(), 0u);
  EXPECT_EQ(root_row.value("imageCount").toLongLong(), 0);
  EXPECT_EQ(Folders().AllPhotosCount(), 2);

  Folders().SelectLibraryRoot();
  EXPECT_TRUE(Folders().LibraryRootSelected());
  EXPECT_EQ(Folders().CurrentFolderId(), 0u);
  ASSERT_TRUE(WaitFor([this] {
    return !backend_.library()->QueryReplacing() && backend_.library()->TotalCount() == 0;
  }));
  // The read of the scope has ended: the count stays.
  ProcessEvents(500);
  EXPECT_FALSE(backend_.library()->QueryReplacing());
  EXPECT_EQ(backend_.library()->TotalCount(), 0);

  Folders().SelectFolder(0);
  EXPECT_FALSE(Folders().LibraryRootSelected());
  ASSERT_TRUE(WaitFor([this] {
    return !backend_.library()->QueryReplacing() && backend_.library()->TotalCount() == 2;
  }));

  // A folder other than the root is never the Library row.
  Folders().SelectLibraryRoot();
  const auto trip = RowNamed("Trip");
  ASSERT_TRUE(trip.has_value());
  Folders().SelectFolder(trip->value("folderId").toUInt());
  EXPECT_FALSE(Folders().LibraryRootSelected());

  EXPECT_FALSE(trip->value("mountVolumeKey").toString().isEmpty());
  EXPECT_FALSE(trip->value("mountPoint").toString().isEmpty());
  EXPECT_FALSE(trip->value("mountVolumeRoot").toString().isEmpty());
}
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
  const auto present_ids = Summary().present_element_ids;
  EXPECT_EQ(present_ids.size(), 2u);
  for (const auto id : present_ids) {
    EXPECT_TRUE(Folders().IsMountedPhoto(static_cast<uint>(id)));
  }
  EXPECT_FALSE(Folders().IsMountedPhoto(0));
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
  // A missing mounted photo also keeps Delete disabled: it leaves only through Remove Missing.
  EXPECT_TRUE(Folders().IsMountedPhoto(static_cast<uint>(missing_id)));

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

// #1088: a scan that only stored a new volume name did not save the project file, so the next
// open of the .alcd showed the old name.
TEST_F(FolderControllerMountTest, ScanThatOnlyRenamesTheVolumeSavesTheProjectFile) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  const auto& handler    = backend_.project()->handler();
  const auto  mount_id   = Summary().record.id;
  const auto  volume     = ReadVolumeLabel(disk_ / "Trip").name;
  const auto  saved_name = [this, &handler]() -> std::optional<std::string> {
    const auto workspace = temp_dir_ / "volume_name_unpack";
    std::error_code ec;
    stdfs::remove_all(workspace, ec);
    stdfs::create_directories(workspace);
    stdfs::path db_path;
    stdfs::path meta_path;
    if (!project_pack::UnpackProjectToWorkspace(handler.package_path(), workspace,
                                                "volume_name_unpack", &db_path, &meta_path,
                                                nullptr)) {
      return std::nullopt;
    }
    std::optional<std::string> name;
    {
      ProjectService project(db_path, meta_path, ProjectOpenMode::kLoadExisting);
      const auto     roots = project.GetStorage()->GetMountStore().ReadMountRoots();
      if (roots.size() == 1) {
        name = roots.front().volume_name;
      }
    }
    stdfs::remove_all(workspace, ec);
    return name;
  };

  // The volume had another name when the project file was last saved; nothing on the disk
  // changed since.
  const std::string old_name = volume + " (old)";
  handler.project()->GetStorage()->GetMountStore().UpdateMountVolumeName(mount_id, old_name);
  ASSERT_TRUE(handler.MakeSaveAndPackageJob()(nullptr));
  ASSERT_EQ(saved_name(), old_name);

  ASSERT_NO_FATAL_FAILURE(Synchronize());
  EXPECT_EQ(Summary().record.volume_name, volume);
  EXPECT_EQ(saved_name(), volume);
}

TEST_F(FolderControllerMountTest, MountInsideAMountedFolderIsRefusedWithNoRow) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  const auto rows_before = Folders().Folders().size();
  Folders().MountFolder(QUrl::fromLocalFile(PathToQString(disk_ / "Trip" / "Day1")));
  ASSERT_TRUE(WaitFor([this] { return !backend_.mount_scans()->HasPendingScans(); }));
  ProcessEvents(300);
  EXPECT_EQ(Folders().Folders().size(), rows_before);
  EXPECT_EQ(Mounts().ListMounts().size(), 1u);
}

// ── #1016: user import and scan ─────────────────────────────────────────────

TEST_F(FolderControllerMountTest, UserImportOfAFileInAMountedFolderLeavesOneElement) {
  // Decision D10: one file has one element. The user import gave a mounted file a second,
  // ordinary element.
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  WriteJpeg(temp_dir_ / "Other" / "c.jpg", 3);

  backend_.import_export()->StartImport(
      {PathToQString(disk_ / "Trip" / "a.jpg"), PathToQString(temp_dir_ / "Other" / "c.jpg")});
  ASSERT_TRUE(WaitFor([this] {
    return !backend_.import_export()->ImportRunning() && !backend_.mount_scans()->HasPendingScans();
  }));
  EXPECT_EQ(backend_.import_export()->ImportCompleted(), 1);
  // #1089: the Library snackbar of the finished import reports the file left to the mount.
  EXPECT_EQ(backend_.import_export()->ImportLeftToMounts(), 1);
  ASSERT_TRUE(WaitFor([this] { return ProjectPhotoCount() == 3; }));
  ProcessEvents(300);
  EXPECT_EQ(ProjectPhotoCount(), 3);
  EXPECT_EQ(Summary().present_count, 2u);
}

TEST_F(FolderControllerMountTest, UserImportOfOnlyMountedFilesReportsThemAndRunsNoImport) {
  // #1089: with every file in a mounted folder no import runs, so no import state change
  // reports the result; the signal gives the Library snackbar the count.
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  auto*      import_export = backend_.import_export();
  QSignalSpy left_all(import_export, &ImportExportHandler::importLeftAllToMounts);

  import_export->StartImport({PathToQString(disk_ / "Trip" / "a.jpg"),
                              PathToQString(disk_ / "Trip" / "Day1" / "b.jpg")});
  EXPECT_FALSE(import_export->ImportRunning());
  ASSERT_EQ(left_all.size(), 1);
  EXPECT_EQ(left_all.front().front().toInt(), 2);
  EXPECT_EQ(import_export->ImportLeftToMounts(), 2);

  ProcessEvents(300);
  EXPECT_EQ(ProjectPhotoCount(), 2);
  EXPECT_EQ(Summary().present_count, 2u);

  // The next import with no mounted file clears the count, so its snackbar has no mount line.
  WriteJpeg(temp_dir_ / "Other" / "c.jpg", 3);
  import_export->StartImport({PathToQString(temp_dir_ / "Other" / "c.jpg")});
  EXPECT_EQ(import_export->ImportLeftToMounts(), 0);
  ASSERT_TRUE(WaitFor([&] {
    return !import_export->ImportRunning() && !backend_.mount_scans()->HasPendingScans();
  }));
  EXPECT_EQ(left_all.size(), 1);
  ASSERT_TRUE(WaitFor([this] { return ProjectPhotoCount() == 3; }));
}

TEST_F(FolderControllerMountTest, NoScanRunsFromTheStartToTheFinalizeOfAUserImport) {
  ASSERT_NO_FATAL_FAILURE(MountTrip());
  WriteJpeg(temp_dir_ / "Other" / "c.jpg", 3);
  WriteJpeg(disk_ / "Trip" / "Day1" / "d.jpg", 4);
  auto* scans = backend_.mount_scans();

  backend_.import_export()->StartImport({PathToQString(temp_dir_ / "Other" / "c.jpg")});
  ASSERT_TRUE(backend_.import_export()->ImportRunning());
  EXPECT_TRUE(scans->IsHeld());
  const auto mount_id = Summary().record.id;
  bool       ended    = false;
  ASSERT_TRUE(scans->RequestScan(mount_id, MountScanTrigger::kSynchronize,
                                 [&ended](const MountScanResult&) { ended = true; }));

  bool scan_ran_during_import = false;
  ASSERT_TRUE(WaitFor([&] {
    if (backend_.import_export()->ImportRunning()) {
      scan_ran_during_import = scan_ran_during_import || scans->HasWorker();
      return false;
    }
    return true;
  }));
  EXPECT_FALSE(scan_ran_during_import);
  EXPECT_FALSE(scans->IsHeld());
  ASSERT_TRUE(WaitFor([&ended] { return ended; }));
  ASSERT_TRUE(WaitFor([this] { return ProjectPhotoCount() == 4; }));
}

}  // namespace
}  // namespace alcedo::ui::test
