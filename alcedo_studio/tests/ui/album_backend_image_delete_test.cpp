//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_image_delete_test.cpp
/// @brief Image delete integration tests for ApplicationModuleHost.

#include <QSignalSpy>

#include <filesystem>
#include <memory>
#include <optional>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui::test {
namespace {

using DeleteTests = ApplicationModuleHostTestFixture;

struct SeededProject {
  std::filesystem::path packed_path_{};
  sl_element_id_t       file_id_  = 0;
  image_id_t            image_id_ = 0;
};

void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 30000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int  step    = 200;
  int        elapsed = 0;
  while (backend.import_export()->ImportRunning() && elapsed < timeoutMs) {
    spy.wait(step);
    elapsed += step;
  }
  ProcessEvents(600);
}

auto FindPackedProjectPath(const std::filesystem::path& dir)
    -> std::optional<std::filesystem::path> {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".alcd") {
      return entry.path();
    }
  }
  return std::nullopt;
}

auto FindFolderId(const QVariantList& folders, const QString& name) -> uint {
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == name) {
      return map.value("folderId").toUInt();
    }
  }
  return 0;
}

auto CreateSeededPackedProject(const std::filesystem::path& tempDir)
    -> std::optional<SeededProject> {
  const auto db_path     = tempDir / "album_delete_seed.db";
  const auto meta_path   = tempDir / "album_delete_seed.json";
  const auto packed_path = tempDir / "album_delete_seed.alcd";

  auto project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
  auto image_handle = project->GetImagePoolService()->CreateAndReturnPinnedEmpty();
  if (!image_handle) {
    return std::nullopt;
  }

  auto image         = image_handle.Get();
  image->image_path_ = tempDir / "album-delete.dng";
  image->image_name_ = L"album-delete.dng";
  image->image_type_ = ImageType::DNG;

  ExifDisplayMetaData metadata;
  metadata.model_         = "Synthetic Album Camera";
  metadata.lens_          = "Synthetic 50mm";
  metadata.date_time_str_ = "2026-05-25 10:00:00";
  image->SetExifDisplayMetaData(std::move(metadata));

  auto file = project->GetSleeveService()->Write<std::shared_ptr<SleeveFile>>(
      [image](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
        auto created       = fs.CreateFileInLibrary(image->image_name_);
        created->image_id_ = image->image_id_;
        return created;
      });
  if (!file.second.success_ || !file.first) {
    return std::nullopt;
  }

  const sl_element_id_t file_id  = file.first->element_id_;
  const image_id_t      image_id = image->image_id_;

  const auto image_sync = project->GetImagePoolService()->SyncWithStorage();
  if (!image_sync.failed_images_.empty()) {
    return std::nullopt;
  }
  project->SaveProject(meta_path);

  std::filesystem::path snapshot_path;
  if (!project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr)) {
    return std::nullopt;
  }
  if (!project_pack::CreateLiveDbSnapshot(project, snapshot_path, nullptr)) {
    return std::nullopt;
  }
  const bool packed =
      project_pack::WritePackedProject(packed_path, meta_path, snapshot_path, nullptr);
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  if (!packed) {
    return std::nullopt;
  }

  return SeededProject{packed_path, file_id, image_id};
}

auto LoadPackedProject(ApplicationModuleHost& backend, const std::filesystem::path& packedPath) -> bool {
  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectChanged);
  if (!backend.project()->LoadProject(PathToQString(packedPath))) {
    return false;
  }
  WaitForSignal(project_spy, 15000);
  ProcessEvents(500);
  return backend.project()->ServiceReady();
}

TEST_F(DeleteTests, DeleteImages_RemovesImageAndRelatedRows) {
  QVariantMap result;
  int         shownCountAfterDelete = -1;

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend, "delete_image_rows"));

    auto images = CollectRawTestImages("airplane", 1);
    if (images.empty()) {
      GTEST_SKIP() << "No RAW images available in raw/airplane/";
    }

    backend.import_export()->StartImport(PathsToQStringList(images));
    WaitForImportFinished(backend);
    ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

    const QVariantMap first     = backend.library()->Thumbnails().front().toMap();
    const auto deletedElementId = static_cast<sl_element_id_t>(first.value("elementId").toUInt());
    const auto deletedImageId   = static_cast<image_id_t>(first.value("imageId").toUInt());
    ASSERT_NE(deletedElementId, 0u);
    ASSERT_NE(deletedImageId, 0u);

    // Touch editor once so pipeline/history entries are materialized in normal service flow.
    backend.editor()->OpenEditor(static_cast<uint>(deletedElementId), static_cast<uint>(deletedImageId));
    ProcessEvents(400);

    QVariantList targets;
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(deletedElementId)},
                                  {"imageId", static_cast<uint>(deletedImageId)}});
    result = backend.images()->DeleteImages(targets);
    ProcessEvents(500);

    shownCountAfterDelete = backend.library()->ShownCount();
  }

  EXPECT_TRUE(result.value("success").toBool());
  EXPECT_EQ(result.value("deletedCount").toInt(), 1);
  EXPECT_EQ(result.value("failedCount").toInt(), 0);
  EXPECT_EQ(shownCountAfterDelete, 0);
}

TEST_F(DeleteTests, DeleteImages_BestEffortPartialFailure) {
  QVariantMap result;

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend, "delete_partial_failure"));

    auto images = CollectRawTestImages("airplane", 1);
    if (images.empty()) {
      GTEST_SKIP() << "No RAW images available in raw/airplane/";
    }

    backend.import_export()->StartImport(PathsToQStringList(images));
    WaitForImportFinished(backend);
    ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

    const QVariantMap first     = backend.library()->Thumbnails().front().toMap();
    const auto   validElementId = static_cast<sl_element_id_t>(first.value("elementId").toUInt());
    const auto   deletedImageId = static_cast<image_id_t>(first.value("imageId").toUInt());

    QVariantList targets;
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(validElementId)},
                                  {"imageId", static_cast<uint>(deletedImageId)}});
    targets.push_back(QVariantMap{{"elementId", 999999u}, {"imageId", 999999u}});

    result = backend.images()->DeleteImages(targets);
    ProcessEvents(500);
  }

  EXPECT_TRUE(result.value("success").toBool());
  EXPECT_EQ(result.value("deletedCount").toInt(), 1);
  EXPECT_EQ(result.value("failedCount").toInt(), 1);
}

TEST_F(DeleteTests, DeleteImages_BlockedByInteractionPolicyLock) {
  ApplicationModuleHost backend;
  auto* registry =
      qobject_cast<BackgroundTaskController*>(backend.background_tasks());
  ASSERT_NE(registry, nullptr);

  BackgroundTaskSnapshot snapshot;
  snapshot.kind_            = BackgroundTaskKind::ImageAnalysis;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("analysis");
  snapshot.cancelable_      = false;
  snapshot.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::DeleteImages, 42,
      QStringLiteral("This image is being analyzed and cannot be deleted or removed.")});
  registry->RegisterTask(snapshot);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", 42u}, {"imageId", 7u}});
  const QVariantMap result = backend.images()->DeleteImages(targets);

  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_EQ(result.value("deletedCount").toInt(), 0);
  EXPECT_EQ(result.value("failedCount").toInt(), 1);
  EXPECT_TRUE(result.value("message").toString().contains(QStringLiteral("analyzed")));
}

TEST_F(DeleteTests, AddToAlbumThenDeleteFromAlbum_KeepsRootFile) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
  const auto        file_id   = static_cast<sl_element_id_t>(root_item.value("elementId").toUInt());
  const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());
  ASSERT_EQ(file_id, seeded->file_id_);
  ASSERT_EQ(image_id, seeded->image_id_);
  EXPECT_EQ(root_item.value("scopeType").toString(), "root");
  EXPECT_EQ(root_item.value("fileId").toUInt(), file_id);
  EXPECT_EQ(root_item.value("folderId").toUInt(), 0u);

  backend.folders()->CreateFolder("AlbumA");
  ProcessEvents(500);
  const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "AlbumA");
  ASSERT_NE(album_ui_id, 0u);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", static_cast<uint>(file_id)},
                                {"imageId", static_cast<uint>(image_id)}});
  const QVariantMap add_result = backend.images()->AddImagesToFolder(targets, album_ui_id);
  EXPECT_TRUE(add_result.value("success").toBool());
  EXPECT_EQ(add_result.value("addedCount").toInt(), 1);

  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  ASSERT_EQ(backend.library()->ShownCount(), 1);
  const QVariantMap album_item = backend.library()->Thumbnails().front().toMap();
  EXPECT_EQ(album_item.value("elementId").toUInt(), file_id);
  EXPECT_EQ(album_item.value("fileId").toUInt(), file_id);
  EXPECT_NE(album_item.value("folderId").toUInt(), 0u);
  EXPECT_EQ(album_item.value("scopeType").toString(), "album");

  const QVariantMap unlink_result = backend.images()->DeleteImages(targets);
  ProcessEvents(500);
  EXPECT_TRUE(unlink_result.value("success").toBool());
  EXPECT_EQ(unlink_result.value("deletedCount").toInt(), 1);
  EXPECT_EQ(backend.library()->ShownCount(), 0);

  backend.folders()->SelectFolder(0);
  ProcessEvents(500);
  ASSERT_EQ(backend.library()->ShownCount(), 1);
  const QVariantMap root_after_unlink = backend.library()->Thumbnails().front().toMap();
  EXPECT_EQ(root_after_unlink.value("elementId").toUInt(), file_id);

  (void)image_id;
}

TEST_F(DeleteTests, AddToAlbumTwiceIsIdempotent) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
  const auto        file_id   = static_cast<sl_element_id_t>(root_item.value("elementId").toUInt());
  const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());

  backend.folders()->CreateFolder("Idempotent");
  ProcessEvents(500);
  const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "Idempotent");
  ASSERT_NE(album_ui_id, 0u);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", static_cast<uint>(file_id)},
                                {"imageId", static_cast<uint>(image_id)}});

  // First add should succeed.
  const QVariantMap result1 = backend.images()->AddImagesToFolder(targets, album_ui_id);
  EXPECT_TRUE(result1.value("success").toBool());
  EXPECT_EQ(result1.value("addedCount").toInt(), 1);

  // Second add of the same file to the same album must be idempotent.
  const QVariantMap result2 = backend.images()->AddImagesToFolder(targets, album_ui_id);

  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);

  (void)result2;
  (void)image_id;
}

TEST_F(DeleteTests, DeleteFromRootRemovesFromAllAlbums) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
  const auto        file_id   = static_cast<sl_element_id_t>(root_item.value("elementId").toUInt());
  const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());

  backend.folders()->CreateFolder("Cascade1");
  backend.folders()->CreateFolder("Cascade2");
  ProcessEvents(500);

  const uint album1_ui_id = FindFolderId(backend.folders()->Folders(), "Cascade1");
  const uint album2_ui_id = FindFolderId(backend.folders()->Folders(), "Cascade2");
  ASSERT_NE(album1_ui_id, 0u);
  ASSERT_NE(album2_ui_id, 0u);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", static_cast<uint>(file_id)},
                                {"imageId", static_cast<uint>(image_id)}});

  ASSERT_TRUE(backend.images()->AddImagesToFolder(targets, album1_ui_id).value("success").toBool());
  ASSERT_TRUE(backend.images()->AddImagesToFolder(targets, album2_ui_id).value("success").toBool());

  // Verify file appears in both albums.
  backend.folders()->SelectFolder(album1_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  backend.folders()->SelectFolder(album2_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);

  // Delete from Root (folderId=0).
  backend.folders()->SelectFolder(0);
  ProcessEvents(500);
  ASSERT_EQ(backend.library()->ShownCount(), 1);

  const QVariantMap del_result = backend.images()->DeleteImages(targets);
  ProcessEvents(500);
  EXPECT_TRUE(del_result.value("success").toBool());
  EXPECT_EQ(backend.library()->ShownCount(), 0);

  // Both albums should now show 0 files.
  backend.folders()->SelectFolder(album1_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  backend.folders()->SelectFolder(album2_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);

  (void)image_id;
}

TEST_F(DeleteTests, StatsFilterConsistencyAfterMembershipChange) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
  const auto        file_id   = static_cast<sl_element_id_t>(root_item.value("elementId").toUInt());
  const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());

  backend.folders()->CreateFolder("StatsConsist");
  ProcessEvents(500);
  const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "StatsConsist");
  ASSERT_NE(album_ui_id, 0u);

  // Select the album — should be empty initially.
  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);
  EXPECT_EQ(backend.library()->ShownCount(), backend.stats()->TotalPhotoCount());

  // Add file to album.
  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", static_cast<uint>(file_id)},
                                {"imageId", static_cast<uint>(image_id)}});
  ASSERT_TRUE(backend.images()->AddImagesToFolder(targets, album_ui_id).value("success").toBool());

  // Verify shown count matches DB stats count.
  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);
  EXPECT_EQ(backend.library()->ShownCount(), backend.stats()->TotalPhotoCount());

  // Remove file from album.
  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  const QVariantMap unlink_result = backend.images()->DeleteImages(targets);
  ProcessEvents(500);
  EXPECT_TRUE(unlink_result.value("success").toBool());

  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);
  EXPECT_EQ(backend.library()->ShownCount(), backend.stats()->TotalPhotoCount());

  (void)image_id;
}

TEST_F(DeleteTests, AddToAlbumSurvivesReload) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  const auto file_id   = seeded->file_id_;
  auto       alcd_path = seeded->packed_path_;

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(LoadPackedProject(backend, alcd_path));
    ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

    const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
    const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());

    backend.folders()->CreateFolder("SurviveReload");
    ProcessEvents(500);
    const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "SurviveReload");
    ASSERT_NE(album_ui_id, 0u);

    QVariantList targets;
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(file_id)},
                                  {"imageId", static_cast<uint>(image_id)}});
    ASSERT_TRUE(backend.images()->AddImagesToFolder(targets, album_ui_id).value("success").toBool());

    ASSERT_TRUE(backend.project()->SaveProject());
    ProcessEvents(500);

    // After SaveProject, find the repacked .alcd for reload.
    const auto repacked = FindPackedProjectPath(temp_dir_);
    if (repacked.has_value()) {
      alcd_path = *repacked;
    }

    (void)image_id;
  }

  // Reload from the saved packed file.
  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(LoadPackedProject(backend, alcd_path));

    const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "SurviveReload");
    ASSERT_NE(album_ui_id, 0u);

    backend.folders()->SelectFolder(album_ui_id);
    ProcessEvents(500);
    EXPECT_EQ(backend.library()->ShownCount(), 1);
    EXPECT_FALSE(backend.library()->Thumbnails().isEmpty());
    const QVariantMap album_item = backend.library()->Thumbnails().front().toMap();
    EXPECT_EQ(album_item.value("elementId").toUInt(), static_cast<uint>(file_id));
  }
}

}  // namespace
}  // namespace alcedo::ui::test
