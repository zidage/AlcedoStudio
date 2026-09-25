//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Typed search columns (library_search_and_project_size_plan.md, Phase S3): the Image mapper
// and the AI store write the columns, and search, stats, and the thumbnail filter read them
// instead of the metadata JSON.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "storage/mapper/image/image_search_columns.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"
#include "utils/string/search_text.hpp"

namespace alcedo {
namespace {

using library_search_test::LibraryRootFolderId;
using library_search_test::SearchFileNames;
using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

constexpr const char* kNull = "<null>";

/// Run a query and return the first row as text, with kNull for a NULL cell.
auto QueryFirstRow(ProjectService& project, const std::string& sql) -> std::vector<std::string> {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << "query failed: " << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return {};
  }
  std::vector<std::string> row;
  if (duckdb_row_count(&result) > 0) {
    for (idx_t c = 0; c < duckdb_column_count(&result); ++c) {
      if (duckdb_value_is_null(&result, c, 0)) {
        row.emplace_back(kNull);
        continue;
      }
      char* text = duckdb_value_varchar(&result, c, 0);
      row.emplace_back(text);
      duckdb_free(text);
    }
  }
  duckdb_destroy_result(&result);
  return row;
}

void RunStatement(ProjectService& project, const std::string& sql) {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  ASSERT_EQ(duckdb_query(guard.conn_, sql.c_str(), &result), DuckDBSuccess)
      << sql << ": " << duckdb_result_error(&result);
  duckdb_destroy_result(&result);
}

auto ImageIdOfFile(ProjectService& project, sl_element_id_t file_id) -> image_id_t {
  const auto row = QueryFirstRow(
      project, "SELECT image_id FROM FileImage WHERE file_id = " + std::to_string(file_id));
  return row.empty() ? 0 : static_cast<image_id_t>(std::stoul(row[0]));
}

auto BucketCounts(const std::vector<StatsBucket>& buckets) -> std::map<std::string, int> {
  std::map<std::string, int> counts;
  for (const auto& bucket : buckets) {
    counts[bucket.label_] = bucket.count_;
  }
  return counts;
}

auto TwoFileSpecs() -> std::vector<SyntheticImageSpec> {
  return {
      {.file_name_  = L"Nikon-D810-raw00011.NEF",
       .image_path_ = L"D:/photos/z8/Nikon-D810-raw00011.NEF",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON D810",
       .lens_       = "NIKKOR 35mm f/1.8",
       .date_time_  = "2026-06-07 13:12:41",
       .iso_        = 800,
       .aperture_   = 2.8f,
       .focal_      = 35.0f,
       .rating_     = 3},
      {.file_name_  = L"IMG_0067.CR3",
       .image_path_ = L"D:/photos/kyoto/IMG_0067.CR3",
       .make_       = "Canon",
       .model_      = "Canon EOS R5",
       .lens_       = "RF Prime",
       .date_time_  = "",  // no capture date
       .iso_        = 0,   // unknown ISO
       .aperture_   = 5.6f,
       .focal_      = 0.0f,
       .rating_     = 0},
  };
}

class LibrarySearchColumnsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto dir = std::filesystem::temp_directory_path();
    db_path_       = dir / "library_search_columns_test.db";
    meta_path_     = dir / "library_search_columns_test.json";
    RemoveProjectFiles();
  }
  void TearDown() override { RemoveProjectFiles(); }

  void RemoveProjectFiles() {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
};

TEST(SearchTextFoldTest, FoldRemovesSeparatorsAndLowercasesAndKeepsSubstrings) {
  EXPECT_EQ(FoldSearchText(L"Nikon-D810_raw00011.NEF"), L"nikond810raw00011nef");
  EXPECT_EQ(FoldSearchText(L"2026-06-07 13:12:41"), L"20260607131241");
  EXPECT_EQ(FoldSearchText(L"  (EOS) R5!  "), L"eosr5");
  EXPECT_EQ(FoldSearchText(L"\u98CE\u666F \u5C71"), L"\u98CE\u666F\u5C71");  // CJK kept
  EXPECT_EQ(FoldSearchText(L"_-./"), L"");
  EXPECT_EQ(FoldSearchTextUtf8("DSC_0431_dng.dng"), "dsc0431dngdng");
  // A substring of a text folds to a substring of the folded text.
  EXPECT_NE(FoldSearchText(L"Nikon-D810_raw00011.NEF").find(FoldSearchText(L"D810_raw")),
            std::wstring::npos);
}

TEST(SearchTextFoldTest, FoldWordsKeepsOneSpaceBetweenWordsAndMatchesTheFoldWithoutSpaces) {
  EXPECT_EQ(FoldSearchWords(L"NIKKOR Z 85mm f/1.8 S"), L"nikkor z 85mm f 1 8 s");
  EXPECT_EQ(FoldSearchWords(L"  __Nikon-D810_raw00011.NEF--  "), L"nikon d810 raw00011 nef");
  EXPECT_EQ(FoldSearchWords(L"_-./"), L"");
  EXPECT_EQ(FoldSearchWordsUtf8("DSC_0431_dng.dng"), "dsc 0431 dng dng");
  for (const auto* text : {L"NIKKOR Z 85mm f/1.8 S", L"2026:06:07 13:12:41", L"(EOS) R5!"}) {
    auto words = FoldSearchWords(text);
    std::erase(words, L' ');
    EXPECT_EQ(words, FoldSearchText(text));
  }
}

TEST(CaptureDateTimeTest, ParsesExifDateFormsAndRejectsInvalidDates) {
  const auto dash = ParseCaptureDateTime("2026-06-07 13:12:41");
  ASSERT_TRUE(dash.has_value());
  EXPECT_EQ(dash->date_, "2026-06-07");
  EXPECT_EQ(dash->date_time_, "2026-06-07 13:12:41");

  const auto colon = ParseCaptureDateTime("2024:02:29 00:00:05");  // EXIF form, leap day
  ASSERT_TRUE(colon.has_value());
  EXPECT_EQ(colon->date_time_, "2024-02-29 00:00:05");

  const auto date_only = ParseCaptureDateTime("2025-12-31");
  ASSERT_TRUE(date_only.has_value());
  EXPECT_EQ(date_only->date_time_, "2025-12-31 00:00:00");

  for (const auto* invalid : {"", "2026-06", "0000:00:00 00:00:00", "2023-02-29 10:00:00",
                              "2026-13-01 10:00:00", "2026-06-31 10:00:00", "2026-06-07 24:00:00",
                              "2026-06-07 10:00", "20260607", "abcd-ef-gh"}) {
    EXPECT_FALSE(ParseCaptureDateTime(invalid).has_value()) << invalid;
  }
}

TEST(ImageSearchColumnsFillTest, DerivesTypedValuesAndFoldedTextFromMetadata) {
  ExifDisplayMetaData metadata;
  metadata.make_          = "NIKON CORPORATION";
  metadata.model_         = "NIKON Z 8";
  metadata.lens_          = "NIKKOR Z 24-120mm f/4 S";
  metadata.lens_make_     = "Nikon";
  metadata.date_time_str_ = "2026-06-07 13:12:41";
  metadata.iso_           = 0;
  metadata.aperture_      = 2.8f;
  metadata.focal_         = 35.0f;
  metadata.rating_        = 9;  // out of range: normalized to 5
  metadata.width_         = 8256;
  metadata.height_        = 5504;

  ImageMapperParams row;
  FillImageSearchColumns(L"DSC_0431_dng.DNG", std::filesystem::path(L"D:/photos/z8/x.DNG"),
                         metadata, row);
  EXPECT_EQ(row.file_stem_, "DSC_0431_dng");
  EXPECT_EQ(row.file_ext_, "dng");
  EXPECT_EQ(row.capture_at_, "2026-06-07 13:12:41");
  EXPECT_EQ(row.capture_date_, "2026-06-07");
  EXPECT_EQ(row.camera_make_, "NIKON CORPORATION");
  EXPECT_EQ(row.camera_model_, "NIKON Z 8");
  EXPECT_EQ(row.lens_, "NIKKOR Z 24-120mm f/4 S");
  EXPECT_FALSE(row.iso_.has_value());  // 0 means unknown
  ASSERT_TRUE(row.aperture_.has_value());
  EXPECT_EQ(*row.aperture_, 2.8);  // not the widened float 2.7999999523
  ASSERT_TRUE(row.focal_mm_.has_value());
  EXPECT_EQ(*row.focal_mm_, 35.0);
  EXPECT_EQ(row.rating_, 5);
  ASSERT_TRUE(row.pixel_count_.has_value());
  EXPECT_EQ(*row.pixel_count_, int64_t{8256} * 5504);
  EXPECT_EQ(row.file_search_text_, "dsc0431dngdng z8");
  EXPECT_EQ(row.exif_search_text_,
            "nikoncorporation nikonz8 nikkorz24120mmf4s nikon 20260607131241");
  // Phase S4: the EXIF words form keeps one space between words and `|` between parts.
  EXPECT_EQ(row.exif_search_words_,
            "nikon corporation|nikon z 8|nikkor z 24 120mm f 4 s|nikon|2026 06 07 13 12 41");

  // An unparsable date leaves both capture columns empty (written as NULL).
  metadata.date_time_str_ = "0000:00:00 00:00:00";
  FillImageSearchColumns(L"a.nef", std::filesystem::path(L"a.nef"), metadata, row);
  EXPECT_TRUE(row.capture_at_.empty());
  EXPECT_TRUE(row.capture_date_.empty());
  EXPECT_EQ(row.file_search_text_, "anef");  // no parent folder
}

TEST_F(LibrarySearchColumnsTest, ImageRowStoresSearchColumnsAndRewritesThemOnUpdate) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              file_ids = builder.AddFiles(TwoFileSpecs());
  ASSERT_EQ(file_ids.size(), 2u);
  const auto        nikon_image = ImageIdOfFile(project, file_ids[0]);
  const auto        canon_image = ImageIdOfFile(project, file_ids[1]);

  const std::string columns =
      "SELECT file_stem, file_ext, CAST(capture_at AS VARCHAR), CAST(capture_date AS VARCHAR), "
      "camera_make, camera_model, lens, iso, focal_mm, aperture, rating, file_search_text, "
      "exif_search_text FROM Image WHERE id = ";
  EXPECT_EQ(
      QueryFirstRow(project, columns + std::to_string(nikon_image)),
      (std::vector<std::string>{"Nikon-D810-raw00011", "nef", "2026-06-07 13:12:41", "2026-06-07",
                                "NIKON CORPORATION", "NIKON D810", "NIKKOR 35mm f/1.8", "800",
                                "35.0", "2.8", "3", "nikond810raw00011nef z8",
                                "nikoncorporation nikond810 nikkor35mmf18 20260607131241"}));
  EXPECT_EQ(QueryFirstRow(project, columns + std::to_string(canon_image)),
            (std::vector<std::string>{"IMG_0067", "cr3", kNull, kNull, "Canon", "Canon EOS R5",
                                      "RF Prime", kNull, kNull, "5.6", "0", "img0067cr3 kyoto",
                                      "canon canoneosr5 rfprime"}));

  // An update through the image pool rewrites the derived columns in the same row write.
  auto image_pool = project.GetImagePoolService();
  image_pool->Write_NoSync<void>(nikon_image, [](const std::shared_ptr<Image>& image) {
    ExifDisplayMetaData metadata = image->exif_display_;
    metadata.model_              = "NIKON Z 8";
    metadata.date_time_str_      = "2026-07-01 08:00:00";
    image->SetExifDisplayMetaData(std::move(metadata));
  });
  image_pool->SyncWithStorage();
  EXPECT_EQ(QueryFirstRow(project,
                          "SELECT camera_model, CAST(capture_date AS VARCHAR), exif_search_text "
                          "FROM Image WHERE id = " +
                              std::to_string(nikon_image)),
            (std::vector<std::string>{"NIKON Z 8", "2026-07-01",
                                      "nikoncorporation nikonz8 nikkor35mmf18 20260701080000"}));

  EXPECT_EQ(QueryFirstRow(project, "SELECT exif_search_words FROM Image WHERE id = " +
                                       std::to_string(nikon_image)),
            (std::vector<std::string>{
                "nikon corporation|nikon z 8|nikkor 35mm f 1 8|2026 07 01 08 00 00"}));

  // The Image still reads back from the 20-column row through the mapper.
  const auto stored = project.GetStorage()->GetImageStore().GetImageById(nikon_image);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->exif_display_.model_, "NIKON Z 8");
  EXPECT_EQ(stored->image_name_, L"Nikon-D810-raw00011.NEF");
}

// The EXIF star-rating path (ImageController::ApplyStarRatingLight, then
// FlushPendingStarRatings) writes the rating with Write_NoSync and one SyncWithStorage.
TEST_F(LibrarySearchColumnsTest, StarRatingWriteUpdatesRatingColumnStatsAndRatingFilter) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              file_ids = builder.AddFiles(TwoFileSpecs());
  ASSERT_EQ(file_ids.size(), 2u);
  const auto canon_image = ImageIdOfFile(project, file_ids[1]);

  auto       image_pool  = project.GetImagePoolService();
  image_pool->Write_NoSync<void>(canon_image, [](const std::shared_ptr<Image>& image) {
    ExifDisplayMetaData metadata;
    if (image->has_exif_display_.load()) {
      metadata = image->exif_display_;
    } else if (image->has_exif_json_.load()) {
      metadata.FromJson(image->exif_json_);
    }
    metadata.rating_ = ExifDisplayMetaData::NormalizeRating(4);
    image->SetExifDisplayMetaData(std::move(metadata));
  });
  // Before the flush the row keeps the old rating.
  EXPECT_EQ(
      QueryFirstRow(project, "SELECT rating FROM Image WHERE id = " + std::to_string(canon_image)),
      std::vector<std::string>{"0"});
  image_pool->SyncWithStorage();
  EXPECT_EQ(
      QueryFirstRow(project, "SELECT rating FROM Image WHERE id = " + std::to_string(canon_image)),
      std::vector<std::string>{"4"});

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);
  const auto          stats     = filter_service.BuildFolderStats(folder_id, std::nullopt);
  EXPECT_EQ(BucketCounts(stats.rating_stats_), (std::map<std::string, int>{{"3", 1}, {"4", 1}}));

  const auto four_stars =
      filter_service.BuildFolderStats(folder_id, sleeve_filter::BuildRatingBucketFilter(L"4"));
  EXPECT_EQ(four_stars.total_photo_count_, 1);
  EXPECT_EQ(BucketCounts(four_stars.camera_stats_),
            (std::map<std::string, int>{{"Canon EOS R5", 1}}));
}

// Acceptance: search, stats, and the thumbnail filter do not read the metadata JSON. The
// metadata column is cleared after the rows are written; every result still comes from the
// typed columns.
TEST_F(LibrarySearchColumnsTest, SearchStatsAndFiltersGiveSameResultsWithMetadataJsonCleared) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  ASSERT_EQ(builder.AddFiles(TwoFileSpecs()).size(), 2u);
  RunStatement(project, "UPDATE Image SET metadata = '{}'");

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);
  const std::string   nikon     = "Nikon-D810-raw00011.NEF";
  const std::string   canon     = "IMG_0067.CR3";

  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"d810"), std::set<std::string>{nikon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"eos r5"), std::set<std::string>{canon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"2026-06-07"),
            std::set<std::string>{nikon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"2026.6"), std::set<std::string>{nikon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"kyoto"), std::set<std::string>{canon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"iso800"), std::set<std::string>{nikon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"f2.8"), std::set<std::string>{nikon});
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"35mm"), std::set<std::string>{nikon});
  // Phase S4: a bare number is a text term; it does not match the ISO column.
  EXPECT_TRUE(SearchFileNames(filter_service, folder_id, L"800").empty());
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"nikkor", kAllSearchFields), 1u);

  const auto stats = filter_service.BuildFolderStats(folder_id, std::nullopt);
  EXPECT_EQ(stats.total_photo_count_, 2);
  EXPECT_EQ(BucketCounts(stats.camera_stats_),
            (std::map<std::string, int>{{"NIKON D810", 1}, {"Canon EOS R5", 1}}));
  EXPECT_EQ(BucketCounts(stats.lens_stats_),
            (std::map<std::string, int>{{"NIKKOR 35mm f/1.8", 1}, {"RF Prime", 1}}));
  EXPECT_EQ(BucketCounts(stats.rating_stats_), (std::map<std::string, int>{{"3", 1}, {"0", 1}}));
  ASSERT_EQ(stats.date_stats_.size(), 2u);
  EXPECT_EQ(BucketCounts(stats.date_stats_).at("2026-06-07"), 1);

  const auto by_date = filter_service.BuildFolderStats(
      folder_id, sleeve_filter::BuildCaptureDateBucketFilter(L"2026-06-07"));
  EXPECT_EQ(by_date.total_photo_count_, 1);
  const auto no_date =
      filter_service.BuildFolderStats(folder_id, sleeve_filter::BuildCaptureDateUnknownFilter());
  EXPECT_EQ(BucketCounts(no_date.camera_stats_), (std::map<std::string, int>{{"Canon EOS R5", 1}}));
  const auto by_lens =
      filter_service.BuildFolderStats(folder_id, sleeve_filter::BuildLensBucketFilter(L"RF Prime"));
  EXPECT_EQ(by_lens.total_photo_count_, 1);

  // Typed thumbnail filter conditions: ISO range, aperture, and focal length.
  const auto condition_count = [&](FilterField field, CompareOp op, FilterValue value,
                                   std::optional<FilterValue> second = std::nullopt) {
    FieldCondition cond{.field_ = field, .op_ = op, .value_ = std::move(value)};
    cond.second_value_ = std::move(second);
    const FilterNode node{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
    return filter_service.BuildFolderStats(folder_id, node).total_photo_count_;
  };
  EXPECT_EQ(condition_count(FilterField::ExifISO, CompareOp::BETWEEN, int64_t{400},
                            FilterValue{int64_t{1600}}),
            1);
  EXPECT_EQ(condition_count(FilterField::ExifAperture, CompareOp::EQUALS, 2.8), 1);
  EXPECT_EQ(condition_count(FilterField::ExifFocalLength, CompareOp::GREATER_EQUAL, 35.0), 1);

  // The compiled search predicate itself names no JSON function, metadata cast, or REPLACE.
  const auto where =
      filter_service.BuildFuzzySearchWhere(L"2026-06-07 d810 P263_5860", kAllSearchFields);
  ASSERT_TRUE(where.has_value());
  const auto sql = conv::ToBytes(*where->raw_sql_);
  EXPECT_EQ(sql.find("json_extract"), std::string::npos);
  EXPECT_EQ(sql.find("metadata"), std::string::npos);
  EXPECT_EQ(sql.find("REPLACE("), std::string::npos);
}

TEST_F(LibrarySearchColumnsTest, AiUnderstandingUpsertWritesFoldedCaptionAndTagsSearchText) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              file_ids = builder.AddFiles(TwoFileSpecs());
  ASSERT_EQ(file_ids.size(), 2u);

  AiDescription description;
  description.file_id_     = file_ids[0];
  description.task_id_     = "describe";
  description.provider_id_ = "test_provider";
  description.model_id_    = "test_model";
  description.caption_     = "Sand dunes at dusk";
  description.scene_       = "Desert";
  description.SetTags({"Sahara", "wind-swept dunes"});
  ASSERT_TRUE(project.GetStorage()->GetAiStore().UpsertUnderstanding(description));

  const auto query =
      "SELECT caption_search_text, tags_search_text FROM AiImageUnderstanding "
      "WHERE file_id = " +
      std::to_string(file_ids[0]);
  EXPECT_EQ(QueryFirstRow(project, query),
            (std::vector<std::string>{"sanddunesatdusk desert", "sahara windsweptdunes"}));
  const auto first_read =
      project.GetStorage()->GetAiStore().GetUnderstanding(file_ids[0], "describe");
  ASSERT_TRUE(first_read.has_value());
  EXPECT_EQ(first_read->caption_, "Sand dunes at dusk");
  EXPECT_EQ(first_read->scene_, "Desert");

  // A re-run for the same (file_id, task_id) replaces the search text with the new result.
  description.caption_ = "Night sky";
  description.scene_   = "";
  description.SetTags({"stars"});
  ASSERT_TRUE(project.GetStorage()->GetAiStore().UpsertUnderstanding(description));
  EXPECT_EQ(QueryFirstRow(project, query), (std::vector<std::string>{"nightsky", "stars"}));
  const auto read_back =
      project.GetStorage()->GetAiStore().GetUnderstanding(file_ids[0], "describe");
  ASSERT_TRUE(read_back.has_value());
  EXPECT_EQ(read_back->caption_, "Night sky");
}

TEST_F(LibrarySearchColumnsTest, AiSearchJoinsEachFileOnceAndKeepsCaptionAndTagMasksApart) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              file_ids = builder.AddFiles(TwoFileSpecs());
  ASSERT_EQ(file_ids.size(), 2u);

  // Two active understandings for one file (two task ids).
  for (const auto* task_id : {"describe", "describe_v2"}) {
    AiDescription description;
    description.file_id_     = file_ids[0];
    description.task_id_     = task_id;
    description.provider_id_ = "test_provider";
    description.model_id_    = "test_model";
    description.caption_     = "Lighthouse on a cliff";
    description.SetTags({"seascape"});
    ASSERT_TRUE(project.GetStorage()->GetAiStore().UpsertUnderstanding(description));
  }

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id        = LibraryRootFolderId(project);
  const auto          description_only = static_cast<SearchFieldMask>(SearchField::AiDescription);
  const auto          tags_only        = static_cast<SearchFieldMask>(SearchField::AiTags);

  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"lighthouse", kAllSearchFields), 1u);
  EXPECT_EQ(filter_service.SearchFolder(folder_id, L"lighthouse", 0, 0, kAllSearchFields).size(),
            1u);
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"lighthouse", description_only), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"lighthouse", tags_only), 0u);
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"seascape", tags_only), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, L"seascape", description_only), 0u);

  // The join does not change listing or stats counts of the folder.
  const auto stats = filter_service.BuildFolderStats(
      folder_id, filter_service.BuildFuzzySearchWhere(L"lighthouse", kAllSearchFields));
  EXPECT_EQ(stats.total_photo_count_, 1);
  EXPECT_EQ(filter_service.BuildFolderStats(folder_id, std::nullopt).total_photo_count_, 2);
}

}  // namespace
}  // namespace alcedo
