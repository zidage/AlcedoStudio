//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/import_service.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <unordered_set>

#include "app/project_service.hpp"
#include "import_test_fixation.hpp"
#include "type/type.hpp"

namespace alcedo {
TEST_F(ImportServiceTests, InitTest) {
  {
    ProjectService project(db_path_, meta_path_);
    auto           fs_service = project.GetSleeveService();
    auto           img_pool   = project.GetImagePoolService();

    EXPECT_NO_THROW(std::unique_ptr<ImportService> import_service =
                        std::make_unique<ImportServiceImpl>(fs_service, img_pool));
  }
}

TEST_F(ImportServiceTests, ImportEmptyTest) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service       = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();

  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  std::vector<image_path_t>  empty_paths;

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();

  ImportResult               final_result;
  bool                       job_finished = false;

  import_job->on_finished_ = [&final_result, &job_finished](const ImportResult& result) {
    final_result = result;
    job_finished = true;
  };

  import_job = import_service->ImportToFolder(empty_paths, L"", {}, import_job);

  ASSERT_NE(import_job, nullptr);

  // Wait for the job to finish
  while (!job_finished) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(final_result.requested_, 0);
  EXPECT_EQ(final_result.imported_, 0);
  EXPECT_EQ(final_result.failed_, 0);
}

TEST_F(ImportServiceTests, ImportSingleFileTest) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service       = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();

  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  std::vector<image_path_t> paths;
  paths.push_back(TEST_IMG_PATH "/raw/airplane/_DSC1704.NEF");

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();

  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();

  import_job->on_progress_                       = [](const ImportProgress& progress) {
    // Can log progress here if needed
    std::cout << "Progress: " << progress.metadata_done_ << "/" << progress.total_ << "\n";
  };

  import_job->on_finished_ = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

  ASSERT_NE(import_job, nullptr);

  // Wait for the job to finish
  final_result_future.wait();

  ImportResult final_result_value = final_result_future.get();

  EXPECT_EQ(final_result_value.requested_, 1);
  EXPECT_EQ(final_result_value.imported_, 1);
  EXPECT_EQ(final_result_value.failed_, 0);

  ASSERT_NE(import_job->import_log_, nullptr);
  auto snapshot = import_job->import_log_->Snapshot();
  EXPECT_EQ(snapshot.created_.size(), 1u);
  EXPECT_EQ(snapshot.metadata_ok_.size(), 1u);
  EXPECT_EQ(snapshot.metadata_failed_.size(), 0u);

  import_service->SyncImports(snapshot, L"");
}

TEST_F(ImportServiceTests, ImportInvalidFileTest) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service       = project.GetSleeveService();

  auto                           img_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  std::vector<image_path_t> paths;
  paths.push_back(TEST_IMG_PATH "/raw/airplane/invalid_file.txt");

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();

  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();

  import_job->on_progress_                       = [](const ImportProgress& progress) {
    // Can log progress here if needed
    std::cout << "Progress: " << progress.metadata_done_ << "/" << progress.total_ << "\n";
  };

  import_job->on_finished_ = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

  ASSERT_NE(import_job, nullptr);

  // Wait for the job to finish
  final_result_future.wait();

  ImportResult final_result_value = final_result_future.get();

  EXPECT_EQ(final_result_value.requested_, 1);
  EXPECT_EQ(final_result_value.imported_, 0);
  EXPECT_EQ(final_result_value.failed_, 1);

  ASSERT_NE(import_job->import_log_, nullptr);
  auto snapshot = import_job->import_log_->Snapshot();
  EXPECT_EQ(snapshot.created_.size(), 0u);
  EXPECT_EQ(snapshot.metadata_ok_.size(), 0u);
  EXPECT_EQ(snapshot.metadata_failed_.size(), 0u);

  EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
}

static auto on_progress_logger = [](const ImportProgress& progress) {
  const uint32_t     total     = progress.total_ ? progress.total_ : 1;  // avoid div by zero
  const uint32_t     done      = progress.metadata_done_;
  const uint32_t     pct       = static_cast<uint32_t>((done * 100) / total);
  constexpr uint32_t bar_width = 24;
  const uint32_t     filled    = (pct * bar_width) / 100;
  std::string        bar(bar_width, ' ');
  for (uint32_t i = 0; i < filled; ++i) {
    bar[i] = '#';
  }

  // Clear line, show green progress bar with percentage
  std::cout << "\r\033[2K[" << bar << "] " << pct << "%"
            << " | " << done << "/" << total << std::flush;
};

TEST_F(ImportServiceTests, ImportWithNonExistentFiles) {

  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  std::vector<image_path_t> paths;
  paths.push_back(TEST_IMG_PATH "/raw/airplane/non_existent_file.NEF");

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();

  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();

  import_job->on_progress_                       = [](const ImportProgress& progress) {
    // Can log progress here if needed
    std::cout << "Progress: " << progress.metadata_done_ << "/" << progress.total_ << "\n";
  };

  import_job->on_finished_ = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

  ASSERT_NE(import_job, nullptr);

  // Wait for the job to finish
  final_result_future.wait();

  ImportResult final_result_value = final_result_future.get();

  EXPECT_EQ(final_result_value.requested_, 1);
  EXPECT_EQ(final_result_value.imported_, 0);
  EXPECT_EQ(final_result_value.failed_, 1);

  auto snapshot = import_job->import_log_->Snapshot();

  EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
}

TEST_F(ImportServiceTests, BatchReadTest) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);
  std::vector<image_path_t> paths;

  image_path_t              img_dir = TEST_IMG_PATH "/raw/batch_import";
  for (const auto& entry : std::filesystem::directory_iterator(img_dir)) {
    paths.push_back(entry.path());
  }

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();
  import_job->on_progress_                       = on_progress_logger;

  import_job->on_finished_                       = [&final_result](const ImportResult& result) {
    std::cout << std::endl;
    final_result.set_value(result);
  };
  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);
  ASSERT_NE(import_job, nullptr);
  // Wait for the job to finish
  final_result_future.wait();
  ImportResult final_result_value = final_result_future.get();
  EXPECT_EQ(final_result_value.requested_, static_cast<uint32_t>(paths.size()));
  EXPECT_EQ(final_result_value.imported_, static_cast<uint32_t>(paths.size()));
  EXPECT_EQ(final_result_value.failed_, 0);

  auto snapshot = import_job->import_log_->Snapshot();
  EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
}

TEST_F(ImportServiceTests, BatchCancelTest) {

  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  std::vector<image_path_t> paths;

  image_path_t              img_dir = TEST_IMG_PATH "/raw/batch_import";
  for (const auto& entry : std::filesystem::directory_iterator(img_dir)) {
    paths.push_back(entry.path());
  }

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();
  import_job->on_progress_                       = on_progress_logger;
  import_job->on_finished_                       = [&final_result](const ImportResult& result) {
    std::cout << std::endl;
    final_result.set_value(result);
  };
  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

  // Sleep for a short while and then cancel
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  import_job->canceled_.store(true);

  ASSERT_NE(import_job, nullptr);
  // Wait for the job to finish
  final_result_future.wait();
  ImportResult final_result_value = final_result_future.get();
  EXPECT_EQ(final_result_value.requested_, static_cast<uint32_t>(paths.size()));
  EXPECT_LT(final_result_value.imported_, static_cast<uint32_t>(paths.size()));
  EXPECT_EQ(final_result_value.failed_, 0);
  std::cout << "This test may fail occasionally in Release mode due to fast imports." << std::endl;

  auto snapshot = import_job->import_log_->Snapshot();
  EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
}

TEST_F(ImportServiceTests, ImportWithInvalidFilesTest) {
  // Create several text files to the test directory
  image_path_t  img_dir           = TEST_IMG_PATH "/raw/batch_import";
  constexpr int num_invalid_files = 5;

  for (int i = 0; i < num_invalid_files; ++i) {
    std::ofstream ofs(img_dir.string() + "/invalid_file_" + std::to_string(i) + ".txt");
    ofs << "This is an invalid image file." << std::endl;
    ofs.close();
  }

  try {

    std::vector<image_path_t>      paths;
    ProjectService                 project(db_path_, meta_path_);
    auto                           fs_service = project.GetSleeveService();
    auto image_pool_service = project.GetImagePoolService();
    std::unique_ptr<ImportService> import_service =
        std::make_unique<ImportServiceImpl>(fs_service, image_pool_service);
    for (const auto& entry : std::filesystem::directory_iterator(img_dir)) {
      paths.push_back(entry.path());
    }
    std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> final_result;
    auto                       final_result_future = final_result.get_future();
    import_job->on_progress_                       = on_progress_logger;

    import_job->on_finished_                       = [&final_result](const ImportResult& result) {
      std::cout << std::endl;
      final_result.set_value(result);
    };
    import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

    ASSERT_NE(import_job, nullptr);
    // Wait for the job to finish
    final_result_future.wait();
    ImportResult final_result_value = final_result_future.get();
    EXPECT_EQ(final_result_value.requested_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(final_result_value.imported_,
              static_cast<uint32_t>(paths.size()) - num_invalid_files);
    EXPECT_EQ(final_result_value.failed_, num_invalid_files);

    ASSERT_NE(import_job->import_log_, nullptr);
    auto snapshot = import_job->import_log_->Snapshot();
    EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
  } catch (const std::exception& e) {
    FAIL() << "Failed to create SleeveManager: " << e.what();
  }

  // Clean up the created text files
  for (int i = 0; i < num_invalid_files; ++i) {
    std::filesystem::remove(img_dir.string() + "/invalid_file_" + std::to_string(i) + ".txt");
  }
}

TEST_F(ImportServiceTests, ImportPartialSuccessWithMockRawFiles) {
  image_path_t  img_dir                = TEST_IMG_PATH "/raw/batch_import";
  constexpr int num_mock_invalid_files = 3;

  for (int i = 0; i < num_mock_invalid_files; ++i) {
    std::ofstream ofs(img_dir.string() + "/mock_invalid_" + std::to_string(i) + ".NEF");
    ofs << "Mock RAW file with invalid content." << std::endl;
    ofs.close();
  }

  try {

    std::vector<image_path_t>      paths;
    ProjectService                 project(db_path_, meta_path_);
    auto                           fs_service = project.GetSleeveService();
    auto                           image_pool_service = project.GetImagePoolService();
    std::unique_ptr<ImportService> import_service =
        std::make_unique<ImportServiceImpl>(fs_service, image_pool_service);
    for (const auto& entry : std::filesystem::directory_iterator(img_dir)) {
      paths.push_back(entry.path());
    }
    std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> final_result;
    auto                       final_result_future = final_result.get_future();
    import_job->on_progress_                       = on_progress_logger;

    import_job->on_finished_                       = [&final_result](const ImportResult& result) {
      std::cout << std::endl;
      final_result.set_value(result);
    };
    import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

    ASSERT_NE(import_job, nullptr);
    final_result_future.wait();
    ImportResult final_result_value = final_result_future.get();

    EXPECT_EQ(final_result_value.requested_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(final_result_value.imported_,
              static_cast<uint32_t>(paths.size()) - num_mock_invalid_files);
    EXPECT_EQ(final_result_value.failed_, num_mock_invalid_files);

    ASSERT_NE(import_job->import_log_, nullptr);
    auto snapshot = import_job->import_log_->Snapshot();
    EXPECT_EQ(snapshot.created_.size(), static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(snapshot.metadata_ok_.size(),
              static_cast<uint32_t>(paths.size()) - num_mock_invalid_files);
    EXPECT_EQ(snapshot.metadata_failed_.size(), num_mock_invalid_files);
    EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
  } catch (const std::exception& e) {
    FAIL() << "Failed to create SleeveManager: " << e.what();
  }

  for (int i = 0; i < num_mock_invalid_files; ++i) {
    std::filesystem::remove(img_dir.string() + "/mock_invalid_" + std::to_string(i) + ".NEF");
  }
}

TEST_F(ImportServiceTests, ImportWithDirectories) {
  // Create several directories
  image_path_t  img_dir  = TEST_IMG_PATH "/raw/batch_import";
  constexpr int num_dirs = 3;
  for (int i = 0; i < num_dirs; ++i) {
    std::filesystem::create_directory(img_dir.string() + "/test_dir_" + std::to_string(i));
  }

  try {

    std::vector<image_path_t>      paths;
    ProjectService                 project(db_path_, meta_path_);
    auto                           fs_service = project.GetSleeveService();
    auto                           image_pool_service = project.GetImagePoolService();
    std::unique_ptr<ImportService> import_service =
        std::make_unique<ImportServiceImpl>(fs_service, image_pool_service);
    for (const auto& entry : std::filesystem::directory_iterator(img_dir)) {
      paths.push_back(entry.path());
    }

    std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> final_result;
    auto                       final_result_future = final_result.get_future();
    import_job->on_progress_                       = on_progress_logger;

    import_job->on_finished_                       = [&final_result](const ImportResult& result) {
      std::cout << std::endl;
      final_result.set_value(result);
    };
    import_job = import_service->ImportToFolder(paths, L"", {}, import_job);

    ASSERT_NE(import_job, nullptr);
    // Wait for the job to finish
    final_result_future.wait();
    ImportResult final_result_value = final_result_future.get();
    EXPECT_EQ(final_result_value.requested_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(final_result_value.imported_, static_cast<uint32_t>(paths.size()) - num_dirs);
    EXPECT_EQ(final_result_value.failed_, num_dirs);

    auto snapshot = import_job->import_log_->Snapshot();
    EXPECT_NO_THROW(import_service->SyncImports(snapshot, L""));
  } catch (const std::exception& e) {
    FAIL() << "Failed to create SleeveManager: " << e.what();
  }

  // Clean up the created directories
  for (int i = 0; i < num_dirs; ++i) {
    std::filesystem::remove(img_dir.string() + "/test_dir_" + std::to_string(i));
  }
}

TEST_F(ImportServiceTests, ImportToNonExistentDestination) {

  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service = project.GetSleeveService();
  auto                           image_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, image_pool_service);

  std::vector<image_path_t> paths;
  paths.push_back(TEST_IMG_PATH "/raw/airplane/_DSC1704.NEF");

  image_path_t               non_existent_dest = TEST_IMG_PATH "/raw/non_existent_folder";

  std::shared_ptr<ImportJob> import_job        = std::make_shared<ImportJob>();

  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();

  import_job->on_finished_                       = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, non_existent_dest, {}, import_job);

  ASSERT_NE(import_job, nullptr);

  // Wait for the job to finish
  final_result_future.wait();

  ImportResult final_result_value = final_result_future.get();

  EXPECT_EQ(final_result_value.requested_, 1);
  EXPECT_EQ(final_result_value.imported_, 0);
  EXPECT_EQ(final_result_value.failed_, 1);

  auto snapshot = import_job->import_log_->Snapshot();
  EXPECT_NO_THROW(import_service->SyncImports(snapshot, non_existent_dest));
  EXPECT_TRUE(fs_service->ListFolderEntries(L"/").empty());
}

// Verify that after import, element names and image names match the original
// file names exactly (not "Element#*" or similar placeholders).
TEST_F(ImportServiceTests, ImportedElementNameMatchesFileName) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service       = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();

  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  // Import a single known image
  const std::filesystem::path test_image_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "airplane" / "_DSC1704.NEF";
  const std::wstring expected_name = test_image_path.filename().wstring();

  std::vector<image_path_t>  paths;
  paths.push_back(test_image_path);

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();

  import_job->on_finished_ = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, L"", {}, import_job);
  ASSERT_NE(import_job, nullptr);

  final_result_future.wait();
  ImportResult result = final_result_future.get();
  ASSERT_EQ(result.imported_, 1);

  // Verify the import log entry has the correct file name
  auto snapshot = import_job->import_log_->Snapshot();
  ASSERT_EQ(snapshot.created_.size(), 1u);
  EXPECT_EQ(snapshot.created_[0].file_name_, expected_name)
      << "Import log file_name_ should match the original image filename";

  import_service->SyncImports(snapshot, L"");

  // Verify the element stored in SleeveService has the correct name
  const auto element_id = snapshot.created_[0].element_id_;
  auto       element    = fs_service->Read<std::shared_ptr<SleeveElement>>(
      [element_id](FileSystem& fs) { return fs.Get(element_id); });
  ASSERT_NE(element, nullptr);
  EXPECT_EQ(element->element_name_, expected_name)
      << "Element name should match the original image filename, not a "
         "placeholder like Element#*";

  // Verify the image stored in ImagePoolService has the correct name
  const auto image_id = snapshot.created_[0].image_id_;
  auto       img_name = img_pool_service->Read<std::wstring>(
      image_id,
      [](std::shared_ptr<Image> img) -> std::wstring { return img->image_name_; });
  EXPECT_EQ(img_name, expected_name)
      << "Image name should match the original image filename";
}

TEST_F(ImportServiceTests, ImportToSubfolder_KeepsUnsyncedAndModifiedSetsDisjoint) {
  ProjectService                 project(db_path_, meta_path_);
  auto                           fs_service       = project.GetSleeveService();
  auto                           img_pool_service = project.GetImagePoolService();
  std::unique_ptr<ImportService> import_service =
      std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

  const std::wstring subfolder = L"Imports";
  const auto         created   = fs_service->CreateFolder(L"/", subfolder);
  ASSERT_NE(created.first, nullptr);
  ASSERT_TRUE(created.second.success_);

  std::vector<image_path_t> paths;
  paths.push_back(TEST_IMG_PATH "/raw/airplane/_DSC1704.NEF");

  std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> final_result;
  auto                       final_result_future = final_result.get_future();
  import_job->on_finished_                       = [&final_result](const ImportResult& result) {
    final_result.set_value(result);
  };

  import_job = import_service->ImportToFolder(paths, L"/Imports", {}, import_job);
  ASSERT_NE(import_job, nullptr);

  final_result_future.wait();
  const ImportResult result = final_result_future.get();
  ASSERT_EQ(result.imported_, 1u);
  ASSERT_NE(import_job->import_log_, nullptr);
  const auto imported_name = std::filesystem::path(paths.front()).filename().wstring();

  const auto root_entries_before_sync = fs_service->ListFolderEntries(L"/");
  const auto album_entries_before_sync = fs_service->ListFolderEntries(L"/Imports");
  sl_element_id_t root_file_id = 0;
  sl_element_id_t album_file_id = 0;
  for (const auto& entry : root_entries_before_sync) {
    ASSERT_NE(entry, nullptr);
    if (entry->type_ == ElementType::FILE && entry->element_name_ == imported_name) {
      root_file_id = entry->element_id_;
    }
  }
  for (const auto& entry : album_entries_before_sync) {
    ASSERT_NE(entry, nullptr);
    if (entry->type_ == ElementType::FILE && entry->element_name_ == imported_name) {
      album_file_id = entry->element_id_;
    }
  }
  ASSERT_NE(root_file_id, 0u);
  ASSERT_EQ(album_file_id, root_file_id);

  const auto unsynced_before_sync =
      fs_service->Read<std::vector<std::shared_ptr<SleeveElement>>>([](FileSystem& fs) {
        return fs.GetUnsyncedElements();
      });
  const auto modified_before_sync =
      fs_service->Read<std::vector<std::shared_ptr<SleeveElement>>>([](FileSystem& fs) {
        return fs.GetModifiedElements();
      });

  std::unordered_set<sl_element_id_t> unsynced_ids;
  for (const auto& element : unsynced_before_sync) {
    ASSERT_NE(element, nullptr);
    unsynced_ids.insert(element->element_id_);
  }

  for (const auto& element : modified_before_sync) {
    ASSERT_NE(element, nullptr);
    EXPECT_FALSE(unsynced_ids.contains(element->element_id_))
        << "Element id " << element->element_id_
        << " must not appear in both unsynced and modified sets";
  }

  bool found_unsynced_file  = false;
  bool found_modified_folder = false;
  for (const auto& element : unsynced_before_sync) {
    if (element->type_ == ElementType::FILE &&
        element->element_name_ == std::filesystem::path(paths.front()).filename().wstring()) {
      found_unsynced_file = true;
    }
  }
  for (const auto& element : modified_before_sync) {
    if (element->type_ == ElementType::FOLDER && element->element_name_ == subfolder) {
      found_modified_folder = true;
    }
  }
  EXPECT_TRUE(found_unsynced_file);
  EXPECT_TRUE(found_modified_folder);

  const auto snapshot = import_job->import_log_->Snapshot();
  import_service->SyncImports(snapshot, L"/Imports");

  const auto root_entries_after_sync = fs_service->ListFolderEntries(L"/");
  const auto album_entries_after_sync = fs_service->ListFolderEntries(L"/Imports");
  bool found_root_membership = false;
  bool found_album_membership = false;
  for (const auto& entry : root_entries_after_sync) {
    ASSERT_NE(entry, nullptr);
    if (entry->element_id_ == root_file_id) {
      found_root_membership = true;
    }
  }
  for (const auto& entry : album_entries_after_sync) {
    ASSERT_NE(entry, nullptr);
    if (entry->element_id_ == root_file_id) {
      found_album_membership = true;
    }
  }
  EXPECT_TRUE(found_root_membership);
  EXPECT_TRUE(found_album_membership);

  const auto unsynced_after_sync =
      fs_service->Read<std::vector<std::shared_ptr<SleeveElement>>>([](FileSystem& fs) {
        return fs.GetUnsyncedElements();
      });
  const auto modified_after_sync =
      fs_service->Read<std::vector<std::shared_ptr<SleeveElement>>>([](FileSystem& fs) {
        return fs.GetModifiedElements();
      });

  EXPECT_TRUE(unsynced_after_sync.empty());
  EXPECT_TRUE(modified_after_sync.empty());
}

};  // namespace alcedo
