//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_image_delete_test.cpp
/// @brief Image delete integration tests for ApplicationModuleHost.

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTimer>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "app/project_service.hpp"
#include "sleeve/storage.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
#include "ui/saved_project_invariants.hpp"
#include "../support/storage_test_access.hpp"

namespace alcedo::ui::test {
namespace {

class DeleteTests : public ApplicationModuleHostTestFixture {
 protected:
  /// Checks the storage invariants of each project package that the test saved (#805).
  void TearDown() override {
    ExpectSavedProjectInvariants(temp_dir_);
    ApplicationModuleHostTestFixture::TearDown();
  }
};

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs = 60000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(10);
  }
  return predicate();
}

/// Starts the delete of @p targets and processes events until DeleteImagesFinished. A delete
/// that does not start returns `success: false` with its refusal in `message`.
auto DeleteAndWait(ApplicationModuleHost& backend, const QVariantList& targets) -> QVariantMap {
  QSignalSpy        finished(backend.images(), &ImageController::DeleteImagesFinished);
  const QVariantMap started = backend.images()->StartDeleteImages(targets);
  if (!started.value("started").toBool()) {
    return QVariantMap{{"success", false}, {"message", started.value("message")}};
  }
  if (!WaitUntil([&]() { return finished.count() > 0; })) {
    return QVariantMap{{"success", false}, {"message", QStringLiteral("delete timed out")}};
  }
  return finished.takeFirst().at(0).toMap();
}

auto FindTaskOfKind(ApplicationModuleHost& backend, const QString& kind) -> QVariantMap {
  auto* registry = qobject_cast<BackgroundTaskController*>(backend.background_tasks());
  if (registry == nullptr) {
    return {};
  }
  for (const QVariant& task : registry->Tasks()) {
    const QVariantMap map = task.toMap();
    if (map.value("kind").toString() == kind) {
      return map;
    }
  }
  return {};
}

auto TargetOf(sl_element_id_t element_id, image_id_t image_id) -> QVariantMap {
  return QVariantMap{{"elementId", static_cast<uint>(element_id)},
                     {"imageId", static_cast<uint>(image_id)}};
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

    // Open the workspace session so pipeline/history entries are materialized in normal service
    // flow.
    backend.workspace_router()->OpenEditor(static_cast<uint>(deletedElementId),
                                           static_cast<uint>(deletedImageId));
    ProcessEvents(400);

    QVariantList targets;
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(deletedElementId)},
                                  {"imageId", static_cast<uint>(deletedImageId)}});
    result = DeleteAndWait(backend, targets);
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

    result = DeleteAndWait(backend, targets);
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
  snapshot.kind_            = BackgroundTaskKind::AdjustmentPaste;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("paste");
  snapshot.cancelable_      = false;
  snapshot.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.locks_.push_back(InteractionLock{
      InteractionCapability::DeleteImages, 42,
      QStringLiteral("This image is being analyzed and cannot be deleted or removed.")});
  registry->RegisterTask(snapshot);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", 42u}, {"imageId", 7u}});
  QSignalSpy        finished(backend.images(), &ImageController::DeleteImagesFinished);
  const QVariantMap result = backend.images()->StartDeleteImages(targets);

  EXPECT_FALSE(result.value("started").toBool());
  EXPECT_TRUE(result.value("message").toString().contains(QStringLiteral("analyzed")));
  EXPECT_FALSE(backend.images()->delete_running());
  ProcessEvents(100);
  EXPECT_EQ(finished.count(), 0);
}

TEST_F(DeleteTests, AddToAlbumThenDeleteFromAlbum_KeepsRootFile) {
  const auto populated = CreatePopulatedPackedProject(temp_dir_);
  ASSERT_TRUE(populated.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  const QVariantMap root_item = backend.library()->Thumbnails().front().toMap();
  const auto        file_id   = static_cast<sl_element_id_t>(root_item.value("elementId").toUInt());
  const auto        image_id  = static_cast<image_id_t>(root_item.value("imageId").toUInt());
  ASSERT_EQ(file_id, populated->file_id_);
  ASSERT_EQ(image_id, populated->image_id_);
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

  const QVariantMap unlink_result = DeleteAndWait(backend, targets);
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

/// The editor list check of @p fileId (LibraryModule::RequestEditorListPosition): the
/// occurrence that the query worker answers, or -3 when no answer arrives.
auto EditorListPosition(ApplicationModuleHost& backend, uint fileId) -> qint64 {
  QSignalSpy       answers(backend.library(), &LibraryModule::editorListPositionReady);
  const qulonglong request_id = backend.library()->RequestEditorListPosition(fileId);
  if (request_id == 0 || !WaitUntil([&]() { return answers.count() > 0; })) {
    return -3;
  }
  const auto args = answers.takeFirst();
  return args.at(0).toULongLong() == request_id ? args.at(2).toLongLong() : -3;
}

TEST_F(DeleteTests, EditorListPositionFollowsCollectionMembership) {
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 2);
  ASSERT_TRUE(populated.has_value());
  ASSERT_EQ(populated->images_.size(), 2u);

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
  ProcessEvents(500);
  ASSERT_EQ(backend.library()->ShownCount(), 2);

  const auto first_id  = static_cast<uint>(populated->images_[0].file_id_);
  const auto second_id = static_cast<uint>(populated->images_[1].file_id_);
  EXPECT_GE(EditorListPosition(backend, first_id), 0);
  EXPECT_GE(EditorListPosition(backend, second_id), 0);

  backend.folders()->CreateFolder("AlbumOnlySecond");
  ProcessEvents(500);
  const uint album_ui_id = FindFolderId(backend.folders()->Folders(), "AlbumOnlySecond");
  ASSERT_NE(album_ui_id, 0u);

  QVariantList targets;
  targets.push_back(QVariantMap{{"elementId", second_id},
                                {"imageId", static_cast<uint>(populated->images_[1].image_id_)}});
  const QVariantMap add_result = backend.images()->AddImagesToFolder(targets, album_ui_id);
  ASSERT_TRUE(add_result.value("success").toBool());

  backend.folders()->SelectFolder(album_ui_id);
  ProcessEvents(500);
  ASSERT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(EditorListPosition(backend, second_id), 0);
  EXPECT_EQ(EditorListPosition(backend, first_id), -1);

  backend.folders()->SelectFolder(0);
  ProcessEvents(500);
  EXPECT_GE(EditorListPosition(backend, first_id), 0);
  EXPECT_GE(EditorListPosition(backend, second_id), 0);
}

TEST_F(DeleteTests, AddToAlbumTwiceIsIdempotent) {
  const auto populated = CreatePopulatedPackedProject(temp_dir_);
  ASSERT_TRUE(populated.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
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
  const auto populated = CreatePopulatedPackedProject(temp_dir_);
  ASSERT_TRUE(populated.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
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

  const QVariantMap del_result = DeleteAndWait(backend, targets);
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
  const auto populated = CreatePopulatedPackedProject(temp_dir_);
  ASSERT_TRUE(populated.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
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
  const QVariantMap unlink_result = DeleteAndWait(backend, targets);
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
  const auto populated = CreatePopulatedPackedProject(temp_dir_);
  ASSERT_TRUE(populated.has_value());

  const auto file_id   = populated->file_id_;
  auto       alcd_path = populated->packed_path_;

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

TEST_F(DeleteTests, DeleteOfSelectionLargerThanOnePageDeletesEverySelectedPhoto) {
  // More photos than one library page (1,000 rows), so the grid loads only a part of them.
  constexpr int kPhotoCount = 1050;
  const auto    populated   = CreatePopulatedPackedProject(temp_dir_, {}, kPhotoCount);
  ASSERT_TRUE(populated.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
  auto* library = backend.library();
  ASSERT_EQ(library->TotalCount(), kPhotoCount);
  ASSERT_LT(library->ShownCount(), kPhotoCount);

  library->SelectAllInCurrentView();
  ASSERT_TRUE(WaitUntil([&]() { return library->selection()->SelectedCount() == kPhotoCount; }));

  QSignalSpy finished(backend.library_mutations(),
                      &LibraryMutationOperations::DeleteTargetsFinished);
  QSignalSpy deleted(backend.library_mutations(), &LibraryMutationOperations::ElementsDeleted);
  const QVariantMap started =
      backend.library_mutations()->StartDeleteTargets(library->selection()->SelectedItems());
  ASSERT_TRUE(started.value("started").toBool())
      << started.value("message").toString().toStdString();
  ASSERT_TRUE(WaitUntil([&]() { return finished.count() == 1; }, 180000));

  const QVariantMap result = finished.takeFirst().at(0).toMap();
  EXPECT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("deletedCount").toInt(), kPhotoCount);
  EXPECT_EQ(result.value("failedCount").toInt(), 0);
  ASSERT_EQ(deleted.count(), 1);
  EXPECT_EQ(deleted.takeFirst().at(0).toList().size(), kPhotoCount);
  EXPECT_EQ(library->selection()->SelectedCount(), 0);

  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(library->TotalCount(), 0);
  EXPECT_EQ(library->ShownCount(), 0);
}

TEST_F(DeleteTests, DeleteRunsOnWorkerWhileUiThreadProcessesEventsAndReportsCompletion) {
  const auto populated = CreatePopulatedPackedProject(temp_dir_, {}, 3);
  ASSERT_TRUE(populated.has_value());
  ASSERT_EQ(populated->images_.size(), 3u);

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, populated->packed_path_));
  ASSERT_EQ(backend.library()->TotalCount(), 3);
  auto*      images   = backend.images();

  // A helper thread holds the project database lock, so the delete job waits in its first
  // database write. The lock is released after 30 s at the latest, so a delete that runs on the
  // UI thread fails the test instead of hanging it.
  auto&      database =
      StorageTestAccess::GetDatabase(*backend.project()->handler().project()->GetStorage());
  std::mutex hold_mutex;
  std::condition_variable hold_changed;
  bool                    held    = false;
  bool                    release = false;
  std::thread             holder([&]() {
    auto             guard   = database.GetConnectionGuard();
    auto             db_lock = guard.Lock();
    std::unique_lock lock(hold_mutex);
    held = true;
    hold_changed.notify_all();
    hold_changed.wait_for(lock, std::chrono::seconds(30), [&]() { return release; });
  });
  const auto              release_holder = [&]() {
    {
      std::lock_guard lock(hold_mutex);
      release = true;
    }
    hold_changed.notify_all();
    if (holder.joinable()) {
      holder.join();
    }
  };
  // Joins the helper thread also when an assertion returns early.
  const std::unique_ptr<void, std::function<void(void*)>> join_holder(
      &holder, [&](void*) { release_holder(); });
  {
    std::unique_lock lock(hold_mutex);
    ASSERT_TRUE(hold_changed.wait_for(lock, std::chrono::seconds(10), [&]() { return held; }));
  }

  QSignalSpy    finished(images, &ImageController::DeleteImagesFinished);
  QElapsedTimer start_timer;
  start_timer.start();
  const QVariantList targets{
      TargetOf(populated->images_[0].file_id_, populated->images_[0].image_id_),
      TargetOf(populated->images_[1].file_id_, populated->images_[1].image_id_)};
  const QVariantMap started  = images->StartDeleteImages(targets);
  const qint64      start_ms = start_timer.elapsed();
  EXPECT_TRUE(started.value("started").toBool())
      << started.value("message").toString().toStdString();
  // The call returned while the database was still locked: the delete did not run in it.
  EXPECT_LT(start_ms, 10000);
  EXPECT_EQ(finished.count(), 0);
  EXPECT_TRUE(images->delete_running());

  // The UI thread keeps processing events while the delete waits on the worker.
  int    ticks = 0;
  QTimer timer;
  timer.setInterval(5);
  QObject::connect(&timer, &QTimer::timeout, [&ticks]() { ++ticks; });
  timer.start();
  ProcessEvents(300);
  timer.stop();
  EXPECT_GT(ticks, 0);
  EXPECT_EQ(finished.count(), 0);

  // The running delete is a background task that holds the DeleteImages lock: a second delete
  // does not start, also of another photo.
  const QVariantMap running_task = FindTaskOfKind(backend, QStringLiteral("imageDelete"));
  EXPECT_EQ(running_task.value("state").toString(), QStringLiteral("running"));
  const QVariantMap second = images->StartDeleteImages(
      QVariantList{TargetOf(populated->images_[2].file_id_, populated->images_[2].image_id_)});
  EXPECT_FALSE(second.value("started").toBool());

  release_holder();
  ASSERT_TRUE(WaitUntil([&]() { return finished.count() == 1; }));
  const QVariantMap result = finished.takeFirst().at(0).toMap();
  EXPECT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("deletedCount").toInt(), 2);
  EXPECT_EQ(result.value("failedCount").toInt(), 0);
  EXPECT_FALSE(images->delete_running());
  EXPECT_EQ(FindTaskOfKind(backend, QStringLiteral("imageDelete")).value("state").toString(),
            QStringLiteral("succeeded"));

  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(backend.library()->TotalCount(), 1);
}

}  // namespace
}  // namespace alcedo::ui::test
