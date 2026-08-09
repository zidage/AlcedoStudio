//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_image_delete_test.cpp
/// @brief Image delete integration tests for ApplicationModuleHost.

#include <filesystem>
#include <optional>

#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui::test {
namespace {

using DeleteTests = ApplicationModuleHostTestFixture;

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

    // Open the workspace session so pipeline/history entries are materialized in normal service
    // flow.
    backend.workspace_router()->OpenEditor(static_cast<uint>(deletedElementId),
                                           static_cast<uint>(deletedImageId));
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

TEST_F(DeleteTests, DeleteTargetsRejectsUnacknowledgedImportAtDirectEntryPoint) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend, "delete_direct_import_guard"));

  const auto source_images = CollectRawTestImages("airplane", 1);
  if (source_images.empty()) {
    GTEST_SKIP() << "No RAW images available in raw/airplane/";
  }

  const auto import_dir = temp_dir_ / "direct_delete_import_dataset";
  std::filesystem::create_directories(import_dir);
  std::vector<std::filesystem::path> import_paths;
  import_paths.reserve(32);
  for (int i = 0; i < 32; ++i) {
    const auto destination = import_dir / ("direct_delete_" + std::to_string(i) +
                                           source_images.front().extension().string());
    std::filesystem::copy_file(source_images.front(), destination,
                               std::filesystem::copy_options::overwrite_existing);
    import_paths.push_back(destination);
  }

  backend.import_export()->StartImport(PathsToQStringList(import_paths));

  ImageController::DeleteTarget target;
  target.element_id_ = 1;
  const auto result  = backend.images()->DeleteTargets({target});

  EXPECT_FALSE(result.success_);
  EXPECT_TRUE(result.message_.contains(QStringLiteral("import"))) << result.message_.toStdString();

  backend.import_export()->CancelImport();
  WaitForImportFinished(backend);
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
  auto* registry = qobject_cast<BackgroundTaskController*>(backend.background_tasks());
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
    ASSERT_TRUE(
        backend.images()->AddImagesToFolder(targets, album_ui_id).value("success").toBool());

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
