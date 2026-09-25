//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Import accepts RAW files only and leaves no orphan rows
// (library_search_and_project_size_plan.md, Phase S1).
//
// The RAW cases use the smallest CI RAW fixture under TEST_IMG_PATH/ci_rawfiles and skip when
// it is missing. The image pool and project-load cases need no RAW file.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "app/image_pool_service.hpp"
#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "library_search_test_support.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "support/non_raw_import_files.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/import/import_error_code.hpp"
#include "utils/import/import_log.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

auto RawFixturePath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "ci_rawfiles" /
         "Tag @ryanbreitkreutz - Free files from @signatureeditscoDSC00830.ARW";
}

auto QueryCount(ProjectService& project, const std::string& sql) -> int64_t {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << "Query failed: " << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return -1;
  }
  const auto count = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  return count;
}

auto CountRows(ProjectService& project, const std::string& table) -> int64_t {
  return QueryCount(project, "SELECT COUNT(*) FROM " + table);
}

/// FileImage rows whose image_id has no Image row: a library file that cannot display.
auto CountFileImageRowsWithoutImage(ProjectService& project) -> int64_t {
  return QueryCount(project,
                    "SELECT COUNT(*) FROM FileImage fi LEFT JOIN Image i ON i.id = fi.image_id "
                    "WHERE i.id IS NULL");
}

auto LibraryFileNames(ProjectService& project) -> std::set<std::string> {
  auto                  guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto                  lock  = guard.Lock();
  duckdb_result         result;
  std::set<std::string> names;
  if (duckdb_query(guard.conn_,
                   "SELECT i.file_name FROM FileImage fi JOIN Image i ON i.id = fi.image_id",
                   &result) != DuckDBSuccess) {
    ADD_FAILURE() << "File name query failed: " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return names;
  }
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
    char* value = duckdb_value_varchar(&result, 0, row);
    names.insert(value);
    duckdb_free(value);
  }
  duckdb_destroy_result(&result);
  return names;
}

struct ImportOutcome {
  ImportResult      result_{};
  ImportLogSnapshot snapshot_{};
};

/// Run the folder import the album backend runs: ImportToFolder, wait, then SyncImports.
/// @p before_sync runs after every metadata task finished and before SyncImports, the window
/// in which the library UI keeps reading the image pool.
auto ImportToLibraryRoot(ProjectService& project, const std::vector<image_path_t>& paths,
                         const std::function<void()>& before_sync = {}) -> ImportOutcome {
  ImportServiceImpl import_service(project.GetSleeveService(), project.GetImagePoolService());
  auto              job = std::make_shared<ImportJob>();
  std::promise<ImportResult> finished;
  auto                       finished_future = finished.get_future();
  job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
  job               = import_service.ImportToFolder(paths, L"", {}, job);
  ImportOutcome outcome;
  outcome.result_   = finished_future.get();
  outcome.snapshot_ = job->import_log_->Snapshot();
  if (before_sync) before_sync();
  import_service.SyncImports(outcome.snapshot_, L"");
  return outcome;
}

class ImportRawOnlyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    const auto* test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto  temp_dir  = std::filesystem::temp_directory_path();
    db_path_              = temp_dir / (std::string("import_raw_only_") + test_name + ".db");
    meta_path_            = temp_dir / (std::string("import_raw_only_") + test_name + ".json");
    scratch_dir_          = temp_dir / (std::string("import_raw_only_") + test_name);
    RemoveTestFiles();
    std::filesystem::create_directories(scratch_dir_);
  }

  void TearDown() override { RemoveTestFiles(); }

  void RemoveTestFiles() {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(db_path_.string() + ".wal", ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(scratch_dir_, ec);
  }

  /// Copy the CI RAW fixture into the scratch folder as @p file_name.
  auto CopyRawFixture(const std::string& file_name) -> std::filesystem::path {
    const auto target = scratch_dir_ / file_name;
    std::filesystem::copy_file(RawFixturePath(), target,
                               std::filesystem::copy_options::overwrite_existing);
    return target;
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path scratch_dir_;
};

TEST_F(ImportRawOnlyTest, MixedFolderImportsOnlyRawFilesAndLeavesNoOrphanImageRows) {
  if (!std::filesystem::exists(RawFixturePath())) {
    GTEST_SKIP() << "CI RAW fixture is missing: " << RawFixturePath().string();
  }
  const auto raw_path = CopyRawFixture("camera_raw.ARW");
  const auto jpeg     = scratch_dir_ / "photo.jpg";
  const auto tiff     = scratch_dir_ / "scan.tif";
  const auto xmp      = scratch_dir_ / "camera_raw.xmp";
  const auto mov      = scratch_dir_ / "clip.mov";
  const auto unknown  = scratch_dir_ / "blob.dat";
  test_support::WriteRgbRaster(jpeg, ".jpg");
  test_support::WriteRgbRaster(tiff, ".tif");
  test_support::WriteXmpSidecar(xmp);
  test_support::WriteQuickTimeHeader(mov);
  test_support::WriteUnknownBinary(unknown);

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {raw_path, jpeg, tiff, xmp, mov, unknown});

  EXPECT_EQ(outcome.result_.requested_, 6u);
  EXPECT_EQ(outcome.result_.imported_, 1u);
  EXPECT_EQ(outcome.result_.failed_, 5u);
  ASSERT_EQ(outcome.snapshot_.metadata_failed_.size(), 5u);
  for (const auto& entry : outcome.snapshot_.metadata_failed_) {
    EXPECT_EQ(entry.error_code_, ImportErrorCode::UNSUPPORTED_FORMAT)
        << conv::ToBytes(entry.file_name_);
  }

  // FinishImport queues semantic generation only for entries with metadata_ok_.
  std::set<std::string> ok_entries;
  for (const auto& entry : outcome.snapshot_.created_) {
    if (entry.metadata_ok_) ok_entries.insert(conv::ToBytes(entry.file_name_));
  }
  EXPECT_EQ(ok_entries, std::set<std::string>{"camera_raw.ARW"});

  EXPECT_EQ(CountRows(project, "FileImage"), 1);
  EXPECT_EQ(CountRows(project, "Image"), 1) << "A failed import must not write an Image row";
  EXPECT_EQ(CountFileImageRowsWithoutImage(project), 0);
  EXPECT_EQ(LibraryFileNames(project), std::set<std::string>{"camera_raw.ARW"});
}

TEST_F(ImportRawOnlyTest, ImportDecidesRawByContentNotByFileExtension) {
  if (!std::filesystem::exists(RawFixturePath())) {
    GTEST_SKIP() << "CI RAW fixture is missing: " << RawFixturePath().string();
  }
  const auto raw_as_bin  = CopyRawFixture("raw_content.bin");
  const auto jpeg_as_nef = scratch_dir_ / "jpeg_content.nef";
  const auto jpeg_as_dng = scratch_dir_ / "jpeg_content.dng";
  test_support::WriteRgbRaster(jpeg_as_nef, ".jpg");
  test_support::WriteRgbRaster(jpeg_as_dng, ".jpg");

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {raw_as_bin, jpeg_as_nef, jpeg_as_dng});

  EXPECT_EQ(outcome.result_.imported_, 1u);
  EXPECT_EQ(outcome.result_.failed_, 2u);
  std::set<std::string> failed_names;
  for (const auto& entry : outcome.snapshot_.metadata_failed_) {
    EXPECT_EQ(entry.error_code_, ImportErrorCode::UNSUPPORTED_FORMAT);
    failed_names.insert(conv::ToBytes(entry.file_name_));
  }
  EXPECT_EQ(failed_names, (std::set<std::string>{"jpeg_content.dng", "jpeg_content.nef"}));
  EXPECT_EQ(LibraryFileNames(project), std::set<std::string>{"raw_content.bin"});
  EXPECT_EQ(CountRows(project, "Image"), 1);
}

// An import larger than the image pool capacity (1024) must not lose a finished Image before
// SyncImports writes it. The loss needs one more pool insert after the metadata tasks release
// their pins: here the library grid reads the Image of a file imported earlier.
TEST_F(ImportRawOnlyTest, ImportLargerThanImagePoolCapacityWritesAnImageRowForEveryFile) {
  if (!std::filesystem::exists(RawFixturePath())) {
    GTEST_SKIP() << "CI RAW fixture is missing: " << RawFixturePath().string();
  }
  constexpr uint32_t kFileCount = ImagePoolManager::kDefaultPoolCapacity + 76;
  static_assert(kFileCount == 1100);
  // Hard links keep 1100 files cheap. NTFS allows 1023 links per file, so the links alternate
  // between two local copies of the fixture.
  const std::vector<std::filesystem::path> link_sources = {CopyRawFixture("source_a.bin"),
                                                           CopyRawFixture("source_b.bin")};
  std::vector<image_path_t>                paths;
  paths.reserve(kFileCount);
  for (uint32_t i = 0; i < kFileCount; ++i) {
    const auto      link = scratch_dir_ / ("DSC_" + std::to_string(10000 + i) + ".ARW");
    std::error_code ec;
    std::filesystem::create_hard_link(link_sources[i % link_sources.size()], link, ec);
    if (ec) {
      GTEST_SKIP() << "Hard links to the RAW fixture are unavailable here: " << ec.message();
    }
    paths.push_back(link);
  }

  // A library that already holds one file, reopened so its Image is in storage only.
  {
    ProjectService          project(db_path_, meta_path_);
    SyntheticLibraryBuilder builder(project);
    ASSERT_EQ(builder
                  .AddFiles({SyntheticImageSpec{.file_name_  = L"earlier.ARW",
                                                .image_path_ = L"D:/photos/earlier.ARW"}})
                  .size(),
              1u);
    project.SaveProject(meta_path_);
  }
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  const auto     earlier_image_id =
      static_cast<image_id_t>(QueryCount(project, "SELECT MAX(id) FROM Image"));

  const auto outcome = ImportToLibraryRoot(project, paths, [&project, earlier_image_id] {
    project.GetImagePoolService()->Read<void>(earlier_image_id,
                                              [](const std::shared_ptr<Image>&) {});
  });

  EXPECT_EQ(outcome.result_.imported_, kFileCount);
  EXPECT_EQ(outcome.result_.failed_, 0u);
  EXPECT_EQ(CountRows(project, "FileImage"), kFileCount + 1);
  EXPECT_EQ(CountRows(project, "Image"), kFileCount + 1);
  EXPECT_EQ(CountFileImageRowsWithoutImage(project), 0)
      << "The image pool dropped Images before SyncImports wrote them";
}

// The pool-level rule behind the large import: an Image with a pending write stays in the pool
// past its capacity until SyncWithStorage writes it.
TEST_F(ImportRawOnlyTest, ImagePoolKeepsUnwrittenImagesPastCapacityUntilSync) {
  constexpr uint32_t kImageCount = ImagePoolManager::kDefaultPoolCapacity + 76;
  ProjectService     project(db_path_, meta_path_);
  auto               pool = project.GetImagePoolService();
  for (uint32_t i = 0; i < kImageCount; ++i) {
    auto handle = pool->CreateAndReturnPinnedEmpty();
    ASSERT_TRUE(handle);
    handle->image_name_ = L"unwritten_" + std::to_wstring(i) + L".ARW";
  }  // Each pin is released here, before the sync, like a finished import task.

  const auto status = pool->SyncWithStorage();
  EXPECT_TRUE(status.failed_images_.empty());
  EXPECT_EQ(status.synced_images_.size(), kImageCount);
  EXPECT_EQ(CountRows(project, "Image"), kImageCount);
}

TEST_F(ImportRawOnlyTest, ProjectLoadRemovesImageRowsWithoutLibraryFile) {
  {
    ProjectService          project(db_path_, meta_path_);
    SyntheticLibraryBuilder builder(project);
    ASSERT_EQ(builder
                  .AddFiles({SyntheticImageSpec{.file_name_  = L"kept.ARW",
                                                .image_path_ = L"D:/photos/kept.ARW"}})
                  .size(),
              1u);
    auto pool = project.GetImagePoolService();
    for (int i = 0; i < 3; ++i) {
      auto handle = pool->CreateAndReturnPinnedEmpty();
      ASSERT_TRUE(handle);
      handle->image_name_ = L"orphan_" + std::to_wstring(i) + L".xmp";
    }
    pool->SyncWithStorage();
    ASSERT_EQ(CountRows(project, "Image"), 4);
    ASSERT_EQ(CountRows(project, "FileImage"), 1);
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  EXPECT_EQ(CountRows(reopened, "Image"), 1);
  EXPECT_EQ(CountRows(reopened, "FileImage"), 1);
  EXPECT_EQ(LibraryFileNames(reopened), std::set<std::string>{"kept.ARW"});
}

}  // namespace
}  // namespace alcedo
