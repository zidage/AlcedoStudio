//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Import classifies files by content into RAW and raster images and leaves no orphan rows
// (library_search_and_project_size_plan.md, Phase S1; raster_image_input_plan.md, Phase R4).
//
// The RAW cases use the smallest CI RAW fixture under TEST_IMG_PATH/ci_rawfiles and skip when
// it is missing. The raster cases use tests/resources/raster. The image pool and project-load
// cases need no RAW file.

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
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/raster_color_description.hpp"
#include "library_search_test_support.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "support/non_raw_import_files.hpp"
#include "type/supported_file_type.hpp"
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

/// Image.type and Image.metadata of the row whose file name is @p file_name.
struct ImageRow {
  int64_t        type_ = -1;
  nlohmann::json metadata_;
};

auto QueryImageRow(ProjectService& project, const std::string& file_name) -> ImageRow {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  ImageRow      row;
  const auto    sql =
      "SELECT type, CAST(metadata AS VARCHAR) FROM Image WHERE file_name = '" + file_name + "'";
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << "Query failed: " << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return row;
  }
  if (duckdb_row_count(&result) == 1) {
    row.type_      = duckdb_value_int64(&result, 0, 0);
    char* metadata = duckdb_value_varchar(&result, 1, 0);
    row.metadata_  = nlohmann::json::parse(metadata);
    duckdb_free(metadata);
  }
  duckdb_destroy_result(&result);
  return row;
}

auto RasterFixturePath(const char* name) -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / name;
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
                         const std::function<void()>& before_sync = {},
                         const ImportOptions&         options     = {}) -> ImportOutcome {
  ImportServiceImpl import_service(project.GetSleeveService(), project.GetImagePoolService(),
                                   std::make_shared<PipelineMgmtService>(project.GetStorage()));
  auto              job = std::make_shared<ImportJob>();
  std::promise<ImportResult> finished;
  auto                       finished_future = finished.get_future();
  job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
  job = import_service.ImportToFolder(paths, L"", options, job);
  ImportOutcome outcome;
  outcome.result_   = finished_future.get();
  outcome.snapshot_ = job->import_log_->Snapshot();
  if (before_sync) before_sync();
  import_service.SyncImports(outcome.snapshot_, L"");
  return outcome;
}

class ImportContentClassificationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    const auto* test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto  temp_dir  = std::filesystem::temp_directory_path();
    db_path_              = temp_dir / (std::string("import_content_class_") + test_name + ".db");
    meta_path_            = temp_dir / (std::string("import_content_class_") + test_name + ".json");
    scratch_dir_          = temp_dir / (std::string("import_content_class_") + test_name);
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

  /// Element id of the imported file @p file_name.
  static auto ElementOf(const ImportOutcome& outcome, const std::string& file_name)
      -> sl_element_id_t {
    for (const auto& entry : outcome.snapshot_.created_) {
      if (conv::ToBytes(entry.file_name_) == file_name) return entry.element_id_;
    }
    ADD_FAILURE() << "No import entry for " << file_name;
    return 0;
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

TEST_F(ImportContentClassificationTest, NikonCfaAndCameraTaggedRgbTiffImportAsDistinctTypes) {
  const auto raw        = RasterFixturePath("nikon_cfa_without_color_matrix.tif");
  const auto tiff       = RasterFixturePath("nikon_rgb_with_camera_metadata.tif");
  const auto raw_as_nef = scratch_dir_ / "nikon_cfa.nef";
  const auto raw_as_bin = scratch_dir_ / "nikon_cfa.bin";
  std::filesystem::copy_file(raw, raw_as_nef);
  std::filesystem::copy_file(raw, raw_as_bin);
  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {raw_as_nef, raw_as_bin, tiff});
  EXPECT_EQ(outcome.result_.requested_, 3u);
  EXPECT_EQ(outcome.result_.imported_, 3u);
  EXPECT_EQ(outcome.result_.failed_, 0u);
  for (const auto* name : {"nikon_cfa.nef", "nikon_cfa.bin"}) {
    const auto row = QueryImageRow(project, name);
    EXPECT_EQ(row.type_, static_cast<int64_t>(ImageType::DEFAULT));
    EXPECT_TRUE(row.metadata_.contains("RawRuntimeColorContext"));
    EXPECT_FALSE(row.metadata_.contains("RasterColorDescription"));
  }
  const auto tiff_row = QueryImageRow(project, tiff.filename().string());
  EXPECT_EQ(tiff_row.type_, static_cast<int64_t>(ImageType::TIFF));
  EXPECT_TRUE(tiff_row.metadata_.contains("RasterColorDescription"));
  EXPECT_FALSE(tiff_row.metadata_.contains("RawRuntimeColorContext"));
  EXPECT_EQ(CountFileImageRowsWithoutImage(project), 0);
}

TEST_F(ImportContentClassificationTest,
       MixedFolderImportsRawAndRasterFilesAndLeavesNoOrphanImageRows) {
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
  EXPECT_EQ(outcome.result_.imported_, 3u);
  EXPECT_EQ(outcome.result_.failed_, 3u);
  ASSERT_EQ(outcome.snapshot_.metadata_failed_.size(), 3u);
  for (const auto& entry : outcome.snapshot_.metadata_failed_) {
    EXPECT_EQ(entry.error_code_, ImportErrorCode::UNSUPPORTED_FORMAT)
        << conv::ToBytes(entry.file_name_);
  }

  // FinishImport queues semantic generation only for entries with metadata_ok_.
  std::set<std::string> ok_entries;
  for (const auto& entry : outcome.snapshot_.created_) {
    if (entry.metadata_ok_) ok_entries.insert(conv::ToBytes(entry.file_name_));
  }
  const std::set<std::string> imported{"camera_raw.ARW", "photo.jpg", "scan.tif"};
  EXPECT_EQ(ok_entries, imported);

  EXPECT_EQ(CountRows(project, "FileImage"), 3);
  EXPECT_EQ(CountRows(project, "Image"), 3) << "A failed import must not write an Image row";
  EXPECT_EQ(CountFileImageRowsWithoutImage(project), 0);
  EXPECT_EQ(LibraryFileNames(project), imported);
}

TEST_F(ImportContentClassificationTest, ImportDecidesKindByContentNotByFileExtension) {
  if (!std::filesystem::exists(RawFixturePath())) {
    GTEST_SKIP() << "CI RAW fixture is missing: " << RawFixturePath().string();
  }
  const auto raw_as_bin  = CopyRawFixture("raw_content.bin");
  const auto jpeg_as_nef = scratch_dir_ / "jpeg_content.nef";
  const auto tiff_as_dng = scratch_dir_ / "tiff_content.dng";
  test_support::WriteRgbRaster(jpeg_as_nef, ".jpg");
  test_support::WriteRgbRaster(tiff_as_dng, ".tif");

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {raw_as_bin, jpeg_as_nef, tiff_as_dng});

  EXPECT_EQ(outcome.result_.imported_, 3u);
  EXPECT_EQ(outcome.result_.failed_, 0u);
  EXPECT_EQ(CountRows(project, "Image"), 3);

  const auto raw_row = QueryImageRow(project, "raw_content.bin");
  EXPECT_EQ(raw_row.type_, static_cast<int64_t>(ImageType::DEFAULT));
  EXPECT_TRUE(raw_row.metadata_.contains("RawRuntimeColorContext"));
  EXPECT_FALSE(raw_row.metadata_.contains("RasterColorDescription"));

  const auto jpeg_row = QueryImageRow(project, "jpeg_content.nef");
  EXPECT_EQ(jpeg_row.type_, static_cast<int64_t>(ImageType::JPEG));
  EXPECT_TRUE(jpeg_row.metadata_.contains("RasterColorDescription"));
  EXPECT_FALSE(jpeg_row.metadata_.contains("RawRuntimeColorContext"));

  EXPECT_EQ(QueryImageRow(project, "tiff_content.dng").type_,
            static_cast<int64_t>(ImageType::TIFF));
}

TEST_F(ImportContentClassificationTest,
       ImportedJpegStoresDescriptionInDevelopInputAndRatingFromExif) {
  const auto jpeg = scratch_dir_ / "display_p3.jpg";
  std::filesystem::copy_file(RasterFixturePath("display_p3_icc_8bit.jpg"), jpeg);
  {
    // The vcpkg Exiv2 build has no XMP toolkit, so ratings come from EXIF.
    auto exiv = Exiv2::ImageFactory::open(jpeg.string());
    exiv->readMetadata();
    exiv->exifData()["Exif.Image.Rating"] = static_cast<uint16_t>(4);
    exiv->writeMetadata();
  }

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {jpeg});
  ASSERT_EQ(outcome.result_.imported_, 1u);

  const auto row = QueryImageRow(project, "display_p3.jpg");
  EXPECT_EQ(row.type_, static_cast<int64_t>(ImageType::JPEG));
  EXPECT_EQ(row.metadata_.value("Rating", 0), 4);
  EXPECT_FALSE(row.metadata_.value("IsHDR", false));
  ASSERT_TRUE(row.metadata_.contains("RasterColorDescription"));

  PipelineMgmtService pipelines(project.GetStorage());
  const auto root = pipelines.LoadHistorySnapshot(ElementOf(outcome, "display_p3.jpg")).root_;
  ASSERT_NE(root, nullptr);
  EXPECT_FALSE(root->raw_color_context.has_value());
  ASSERT_NE(root->document.Develop(), nullptr);
  const auto input = root->document.Develop()->Params().RasterInput();
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ(input->profile_override_, "auto");
  EXPECT_EQ(RasterColorDescriptionToJson(input->source_color_),
            row.metadata_["RasterColorDescription"]);
  EXPECT_EQ(root->document.ToJson(),
            CreateDefaultRasterPipelineDocument(input->source_color_).ToJson());
}

TEST_F(ImportContentClassificationTest, ImportedExrIsSceneLinearAndHdr) {
  const auto exr = scratch_dir_ / "render.exr";
  std::filesystem::copy_file(RasterFixturePath("chromaticities_p3_half.exr"), exr);

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {exr});
  ASSERT_EQ(outcome.result_.imported_, 1u);

  const auto row = QueryImageRow(project, "render.exr");
  EXPECT_EQ(row.type_, static_cast<int64_t>(ImageType::EXR));
  EXPECT_TRUE(row.metadata_.value("IsHDR", false));

  PipelineMgmtService pipelines(project.GetStorage());
  const auto          root = pipelines.LoadHistorySnapshot(ElementOf(outcome, "render.exr")).root_;
  ASSERT_NE(root, nullptr);
  const auto input = root->document.Develop()->Params().RasterInput();
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ(input->source_color_.referral_, RasterReferral::SceneLinear);
}

TEST(ImportFileCategoryTest, CategoryForExtensionIgnoresCaseAndKnowsEveryImportType) {
  EXPECT_EQ(CategoryForExtension(".Nef"), ImportFileCategory::Raw);
  EXPECT_EQ(CategoryForExtension(".DNG"), ImportFileCategory::Raw);
  EXPECT_EQ(CategoryForExtension(".x3f"), ImportFileCategory::Raw);
  EXPECT_EQ(CategoryForExtension(".JPEG"), ImportFileCategory::Jpeg);
  EXPECT_EQ(CategoryForExtension(".jfif"), ImportFileCategory::Jpeg);
  EXPECT_EQ(CategoryForExtension(".TIF"), ImportFileCategory::Tiff);
  EXPECT_EQ(CategoryForExtension(".png"), ImportFileCategory::Png);
  EXPECT_EQ(CategoryForExtension(".exr"), ImportFileCategory::OpenExr);
  EXPECT_EQ(CategoryForExtension(".xmp"), ImportFileCategory::Other);
  EXPECT_EQ(CategoryForExtension(""), ImportFileCategory::Other);
  EXPECT_EQ(CategoryForPath(std::filesystem::path(L"photo.\u00e9jpg")), ImportFileCategory::Other);
  EXPECT_EQ(ImportCategoryBit(ImportFileCategory::Other), 0);
  EXPECT_EQ(ImportFileCategory::Raw | ImportFileCategory::Jpeg, 0x03);
  EXPECT_TRUE(CategoryAllowed(kAllImportCategories, ImportFileCategory::OpenExr));
  EXPECT_FALSE(CategoryAllowed(kAllImportCategories, ImportFileCategory::Other));
}

// Section 9.1: the content category decides. A JPEG renamed .nef with only RAW allowed is an
// excluded type, not an unsupported file, and leaves no Image row.
TEST_F(ImportContentClassificationTest, ContentOutsideTheAllowedCategoriesIsCountedAsExcludedType) {
  const auto jpeg_as_nef = scratch_dir_ / "jpeg_content.nef";
  const auto png         = scratch_dir_ / "graphic.png";
  const auto blob        = scratch_dir_ / "blob.dat";
  test_support::WriteRgbRaster(jpeg_as_nef, ".jpg");
  test_support::WriteRgbRaster(png, ".png");
  test_support::WriteUnknownBinary(blob);

  ImportOptions options;
  options.allowed_categories_ =
      ImportCategoryBit(ImportFileCategory::Raw) | ImportFileCategory::Png;
  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {jpeg_as_nef, png, blob}, {}, options);

  EXPECT_EQ(outcome.result_.imported_, 1u);
  EXPECT_EQ(outcome.result_.failed_, 2u);
  EXPECT_EQ(outcome.result_.excluded_type_, 1u);
  EXPECT_EQ(outcome.result_.unsupported_, 1u);
  std::set<std::string> excluded;
  for (const auto& entry : outcome.snapshot_.metadata_failed_) {
    if (entry.error_code_ == ImportErrorCode::EXCLUDED_TYPE) {
      excluded.insert(conv::ToBytes(entry.file_name_));
    }
  }
  EXPECT_EQ(excluded, std::set<std::string>{"jpeg_content.nef"});
  EXPECT_EQ(LibraryFileNames(project), std::set<std::string>{"graphic.png"});
  EXPECT_EQ(CountRows(project, "Image"), 1);
}

TEST_F(ImportContentClassificationTest, CmykJpegIsUnsupportedAndLeavesNoImageRow) {
  const auto cmyk = scratch_dir_ / "print.jpg";
  std::filesystem::copy_file(RasterFixturePath("cmyk_icc.jpg"), cmyk);

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportToLibraryRoot(project, {cmyk});
  EXPECT_EQ(outcome.result_.imported_, 0u);
  ASSERT_EQ(outcome.snapshot_.metadata_failed_.size(), 1u);
  EXPECT_EQ(outcome.snapshot_.metadata_failed_.front().error_code_,
            ImportErrorCode::UNSUPPORTED_FORMAT);
  EXPECT_EQ(CountRows(project, "Image"), 0);
}

// An import larger than the image pool capacity (1024) must not lose a finished Image before
// SyncImports writes it. The loss needs one more pool insert after the metadata tasks release
// their pins: here the library grid reads the Image of a file imported earlier.
TEST_F(ImportContentClassificationTest,
       ImportLargerThanImagePoolCapacityWritesAnImageRowForEveryFile) {
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
TEST_F(ImportContentClassificationTest, ImagePoolKeepsUnwrittenImagesPastCapacityUntilSync) {
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

TEST_F(ImportContentClassificationTest, ProjectLoadRemovesImageRowsWithoutLibraryFile) {
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
