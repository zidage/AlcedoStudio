//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Search recall and precision table for the library fuzzy search, plus the local Nikon
// folder import-and-recall test (library_search_and_project_size_plan.md, Phase S0).
//
// Each case lists the exact file set a user expects. A case marked `kKnownDefect` records a
// defect that the current search has; the test then requires the result to still differ from
// the expected set. When a later phase fixes the defect, the test fails and asks for the flag
// to be cleared, so the table always states the real behavior.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "sleeve/storage.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_model_registry.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo {
namespace {

using library_search_test::CountTableRows;
using library_search_test::LibraryRootFolderId;
using library_search_test::SearchFileNames;
using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

enum class CaseState { kPasses, kKnownDefect };

struct RecallCase {
  std::wstring          query_;
  std::set<std::string> expected_;
  CaseState             state_ = CaseState::kPasses;
};

void ExpectRecallCase(const SleeveFilterService& filter_service, sl_element_id_t folder_id,
                      const RecallCase& recall_case) {
  const auto  actual = SearchFileNames(filter_service, folder_id, recall_case.query_);
  const auto  label  = conv::ToBytes(recall_case.query_);
  std::string actual_text;
  for (const auto& name : actual) actual_text += name + " ";
  if (recall_case.state_ == CaseState::kPasses) {
    EXPECT_EQ(actual, recall_case.expected_) << "query `" << label << "` returned: " << actual_text;
  } else {
    std::cout << "[known defect] `" << label << "` returns: " << actual_text << '\n';
    EXPECT_NE(actual, recall_case.expected_)
        << "query `" << label << "` now returns the expected set. The defect is fixed: "
        << "change its state to kPasses.";
  }
}

// Synthetic library. Paths, dates, and parameters are chosen so that each expected set is
// unambiguous: no file name, date, or time contains "67" except IMG_0067, and only file 4
// has ISO 8000 (to check that `iso800` does not match it).
auto RecallLibrarySpecs() -> std::vector<SyntheticImageSpec> {
  const std::string panasonic = "Panasonic";
  const std::string g9        = "DC-G9M2";
  const std::string lumix     = "LUMIX Standard Zoom";
  return {
      {.file_name_  = L"P2635860.RW2",
       .image_path_ = L"D:/photos/shangrila/P2635860.RW2",
       .make_       = panasonic,
       .model_      = g9,
       .lens_       = lumix,
       .date_time_  = "2026-06-07 13:12:41",
       .iso_        = 800,
       .aperture_   = 4.0f,
       .focal_      = 25.0f},
      {.file_name_  = L"P2635861.RW2",
       .image_path_ = L"D:/photos/shangrila/P2635861.RW2",
       .make_       = panasonic,
       .model_      = g9,
       .lens_       = lumix,
       .date_time_  = "2026-06-07 13:12:54",
       .iso_        = 200,
       .aperture_   = 2.8f,
       .focal_      = 35.0f},
      {.file_name_  = L"P2635912.RW2",
       .image_path_ = L"D:/photos/shangrila/P2635912.RW2",
       .make_       = panasonic,
       .model_      = g9,
       .lens_       = lumix,
       .date_time_  = "2026-06-08 09:10:12",
       .iso_        = 1600,
       .aperture_   = 5.6f,
       .focal_      = 50.0f},
      {.file_name_       = L"DSC_0431_dng.dng",
       .image_path_      = L"D:/photos/iceland/DSC_0431_dng.dng",
       .make_            = "NIKON CORPORATION",
       .model_           = "NIKON Z 7",
       .lens_            = "NIKKOR Z Prime",
       .date_time_       = "2025-06-07 10:20:30",
       .iso_             = 8000,
       .aperture_        = 8.0f,
       .focal_           = 85.0f,
       .has_dng_profile_ = true},
      {.file_name_       = L"DSC_0432.dng",
       .image_path_      = L"D:/photos/iceland/DSC_0432.dng",
       .make_            = "NIKON CORPORATION",
       .model_           = "NIKON Z 7",
       .lens_            = "NIKKOR Z Prime",
       .date_time_       = "2026-07-06 10:20:30",
       .iso_             = 400,
       .aperture_        = 11.0f,
       .focal_           = 24.0f,
       .has_dng_profile_ = true},
      {.file_name_  = L"Nikon-D810-raw00011.nef",
       .image_path_ = L"D:/photos/kyoto/Nikon-D810-raw00011.nef",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON D810",
       .lens_       = "NIKKOR F Wide",
       .date_time_  = "2024-03-15 08:09:10",
       .iso_        = 100,
       .aperture_   = 5.6f,
       .focal_      = 20.0f},
      {.file_name_  = L"nikon_d3x_01.nef",
       .image_path_ = L"D:/photos/kyoto/nikon_d3x_01.nef",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON D3X",
       .lens_       = "NIKKOR F Wide",
       .date_time_  = "2024-03-16 08:09:10",
       .iso_        = 100,
       .aperture_   = 5.6f,
       .focal_      = 20.0f},
      {.file_name_  = L"IMG_0067.CR3",
       .image_path_ = L"D:/photos/kyoto/IMG_0067.CR3",
       .make_       = "Canon",
       .model_      = "Canon EOS R5",
       .lens_       = "RF Prime",
       .date_time_  = "2025-01-02 03:04:05",
       .iso_        = 200,
       .aperture_   = 5.6f,
       .focal_      = 50.0f},
      {.file_name_  = L"P2640001.RW2",
       .image_path_ = L"D:/photos/shangrila/P2640001.RW2",
       .make_       = panasonic,
       .model_      = g9,
       .lens_       = lumix,
       .date_time_  = "2026-05-30 18:20:40",
       .iso_        = 200,
       .aperture_   = 5.6f,
       .focal_      = 50.0f},
  };
}

// Expected sets follow the query rules in the plan (Decision D4, Phase S4): a date, kind, or
// capture parameter term also keeps a text alternative against the folded search text.
auto RecallCases() -> std::vector<RecallCase> {
  const std::string f1 = "P2635860.RW2", f2 = "P2635861.RW2", f3 = "P2635912.RW2",
                    f4 = "DSC_0431_dng.dng", f5 = "DSC_0432.dng", f6 = "Nikon-D810-raw00011.nef",
                    f7 = "nikon_d3x_01.nef", f8 = "IMG_0067.CR3", f9 = "P2640001.RW2";
  constexpr auto passes = CaseState::kPasses;
  return {
      // Dates: month-day forms match June 7 of any year; "67" also matches IMG_0067.
      {L"6.7", {f1, f2, f4, f8}, passes},  // Phase S4: date term
      {L"6/7", {f1, f2, f4, f8}, passes},
      {L"0607", {f1, f2, f4}, passes},  // Phase S2: no DNG profile numbers in metadata
      {L"6月7日", {f1, f2, f4}, passes},
      {L"June 7", {f1, f2, f4}, passes},
      {L"2026.6", {f1, f2, f3}, passes},
      {L"2026-06-07", {f1, f2}, passes},
      {L"20260607", {f1, f2}, passes},
      // File kinds.
      {L"jpg", {}, passes},
      {L"dng", {f4, f5}, passes},  // Phase S3: no metadata dump in the search text
      {L"rw2", {f1, f2, f3, f9}, passes},
      // Every file is RAW: the Phase S4 file kind term matches every RAW extension.
      {L"raw", {f1, f2, f3, f4, f5, f6, f7, f8, f9}, passes},
      // Capture parameters (Phase S4). ISO 8000 (file 4) does not match `iso800`.
      {L"iso800", {f1}, passes},
      {L"f2.8", {f2}, passes},
      {L"35mm", {f2}, passes},
      // File names: whole stems, fragments, and stems split at separators.
      {L"P2635860", {f1}, passes},
      {L"2635860", {f1}, passes},
      {L"5860", {f1}, passes},  // Phase S2: no DNG profile numbers in metadata
      {L"P263 5860", {f1}, passes},
      {L"p26358", {f1, f2}, passes},
      {L"d810 raw 11", {f6}, passes},
      {L"raw00011", {f6}, passes},
      {L"nikond3x01", {f7}, passes},
      {L"d3x_01", {f7}, passes},
      {L"nikon d3x 01", {f7}, passes},
      // Path tail (parent folder name) and camera model.
      {L"shangrila", {f1, f2, f3, f9}, passes},
      {L"G9M2", {f1, f2, f3, f9}, passes},
      {L"eos r5", {f8}, passes},
      // Phase S4 forms beyond the Phase S0 table. Terms combine with AND.
      {L"2026年6月", {f1, f2, f3}, passes},
      {L"June", {f1, f2, f3, f4}, passes},
      {L"7 June", {f1, f2, f4}, passes},
      {L"June 7 2026", {f1, f2}, passes},
      {L"3月", {f6, f7}, passes},
      {L"March 2024", {f6, f7}, passes},
      {L"iso 800", {f1}, passes},
      {L"f/2.8", {f2}, passes},
      {L"35 mm", {f2}, passes},
      {L".nef", {f6, f7}, passes},
      {L"dng 2026", {f5}, passes},
      {L"raw 6.7", {f1, f2, f4, f8}, passes},
      {L"rw2 iso200", {f2, f9}, passes},
  };
}

class LibrarySearchRecallTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto dir = std::filesystem::temp_directory_path();
    db_path_       = dir / "library_search_recall_test.db";
    meta_path_     = dir / "library_search_recall_test.json";
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

TEST_F(LibrarySearchRecallTest, FuzzySearchReturnsExpectedFilesForEachRecallCase) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              specs = RecallLibrarySpecs();
  ASSERT_EQ(builder.AddFiles(specs).size(), specs.size());

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);
  for (const auto& recall_case : RecallCases()) {
    ExpectRecallCase(filter_service, folder_id, recall_case);
  }
}

auto SearchFileNamesWithMask(const SleeveFilterService& filter_service, sl_element_id_t folder_id,
                             const std::wstring& query, SearchFieldMask mask)
    -> std::set<std::string> {
  std::set<std::string> names;
  for (const auto& match : filter_service.SearchFolder(folder_id, query, 0, 0, mask)) {
    names.insert(match.file_name_);
  }
  EXPECT_EQ(filter_service.CountSearchResults(folder_id, query, mask), names.size())
      << conv::ToBytes(query);
  return names;
}

// Phase S4 step 5: the typed part of a term needs the field bit of its column (Exif for dates
// and capture parameters, Filename for file kinds); the text alternative follows the text
// columns that the mask enables.
TEST_F(LibrarySearchRecallTest, TypedTermsMatchOnlyWhenTheirFieldBitIsEnabled) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              specs = RecallLibrarySpecs();
  ASSERT_EQ(builder.AddFiles(specs).size(), specs.size());
  SleeveFilterService         filter_service(project.GetStorage());
  const auto                  folder_id = LibraryRootFolderId(project);

  constexpr auto              file_only = static_cast<SearchFieldMask>(SearchField::Filename);
  constexpr auto              exif_only = static_cast<SearchFieldMask>(SearchField::Exif);
  const std::set<std::string> june_7    = {"P2635860.RW2", "P2635861.RW2", "DSC_0431_dng.dng"};

  // `6.7`: the date is an Exif term; "67" is only in the file name IMG_0067.
  EXPECT_EQ(SearchFileNamesWithMask(filter_service, folder_id, L"6.7", exif_only), june_7);
  EXPECT_EQ(SearchFileNamesWithMask(filter_service, folder_id, L"6.7", file_only),
            (std::set<std::string>{"IMG_0067.CR3"}));
  // `rw2`: the file kind is a Filename term; the EXIF text has no "rw2".
  EXPECT_EQ(SearchFileNamesWithMask(filter_service, folder_id, L"rw2", file_only).size(), 4u);
  EXPECT_TRUE(SearchFileNamesWithMask(filter_service, folder_id, L"rw2", exif_only).empty());
  // `iso800`: the capture parameter is an Exif term.
  EXPECT_EQ(SearchFileNamesWithMask(filter_service, folder_id, L"iso800", exif_only),
            (std::set<std::string>{"P2635860.RW2"}));
  EXPECT_TRUE(SearchFileNamesWithMask(filter_service, folder_id, L"iso800", file_only).empty());
  // AND of terms from different field groups needs both groups.
  EXPECT_EQ(
      SearchFileNamesWithMask(filter_service, folder_id, L"shangrila iso800", kAllSearchFields),
      (std::set<std::string>{"P2635860.RW2"}));
  EXPECT_TRUE(
      SearchFileNamesWithMask(filter_service, folder_id, L"shangrila iso800", file_only).empty());
}

// In the EXIF text, a folded match that crosses a word boundary must not end inside a number:
// `z8` is the model `NIKON Z 8`, not the lens `NIKKOR Z 85mm` (folded `nikkorz85mm`). Inside
// one word a token may still end anywhere, a cross-word match that ends before a letter is
// kept (`nikon_d3` while the user types `nikon_d3x`), and file names keep the plain substring
// rule (`dsc223` while the user types `DSC_2230`).
TEST_F(LibrarySearchRecallTest, CrossWordMatchDoesNotEndInsideANumber) {
  ProjectService                        project(db_path_, meta_path_);
  SyntheticLibraryBuilder               builder(project);
  const std::vector<SyntheticImageSpec> specs = {
      {.file_name_  = L"DSC_2230.NEF",
       .image_path_ = L"D:/photos/trip/DSC_2230.NEF",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON Z 8",
       .lens_       = "NIKKOR Z 70-200mm f/2.8 VR S II",
       .date_time_  = "2026-04-19 18:19:36"},
      {.file_name_  = L"DSC_1456.NEF",
       .image_path_ = L"D:/photos/trip/DSC_1456.NEF",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON Z f",
       .lens_       = "NIKKOR Z 85mm f/1.8 S",
       .date_time_  = "2023-12-29 05:20:49"},
      {.file_name_  = L"nikon_d3x_01.nef",
       .image_path_ = L"D:/photos/trip/nikon_d3x_01.nef",
       .make_       = "NIKON CORPORATION",
       .model_      = "NIKON D3X",
       .lens_       = "NIKKOR F Wide",
       .date_time_  = "2024-03-16 08:09:10"},
  };
  ASSERT_EQ(builder.AddFiles(specs).size(), specs.size());
  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);

  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"z8"),
            (std::set<std::string>{"DSC_2230.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"z85mm"),
            (std::set<std::string>{"DSC_1456.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"z 85"),
            (std::set<std::string>{"DSC_1456.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"nikon_d3"),
            (std::set<std::string>{"nikon_d3x_01.nef"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"dsc223"),
            (std::set<std::string>{"DSC_2230.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"DSC2230"),
            (std::set<std::string>{"DSC_2230.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"70-200"),
            (std::set<std::string>{"DSC_2230.NEF"}));
  EXPECT_TRUE(SearchFileNames(filter_service, folder_id, L"70-20").empty());
}

// The month range of `YYYY.12` ends at January 1 of the next year (not at a month 13).
TEST_F(LibrarySearchRecallTest, YearMonthAndMonthTermsCoverDecember) {
  ProjectService                        project(db_path_, meta_path_);
  SyntheticLibraryBuilder               builder(project);
  const std::vector<SyntheticImageSpec> specs = {
      {.file_name_  = L"A0001.NEF",
       .image_path_ = L"D:/photos/winter/A0001.NEF",
       .date_time_  = "2025-12-31 23:59:59"},
      {.file_name_  = L"A0002.NEF",
       .image_path_ = L"D:/photos/winter/A0002.NEF",
       .date_time_  = "2026-01-01 00:00:00"},
      {.file_name_  = L"A0003.NEF",
       .image_path_ = L"D:/photos/winter/A0003.NEF",
       .date_time_  = "2024-12-01 08:00:00"},
  };
  ASSERT_EQ(builder.AddFiles(specs).size(), specs.size());
  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);

  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"2025.12"),
            (std::set<std::string>{"A0001.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"2025年12月"),
            (std::set<std::string>{"A0001.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"December"),
            (std::set<std::string>{"A0001.NEF", "A0003.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"12.31"),
            (std::set<std::string>{"A0001.NEF"}));
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"2026"),
            (std::set<std::string>{"A0002.NEF"}));
}

/// File names of the rows that @p filter selects in the folder (one page with every row).
auto FilteredFileNames(const SleeveFilterService& filter_service, sl_element_id_t folder_id,
                       const FilterNode& filter) -> std::set<std::string> {
  std::set<std::string> names;
  const auto page = filter_service.ListSearchResultPage(folder_id, filter, 0, 1000);
  for (const auto& row : page.rows_) {
    names.insert(row.file_name_);
  }
  EXPECT_EQ(page.total_, names.size());
  return names;
}

/// A RawSQL filter with one string bind for each `?` in @p sql.
auto RawFilter(const std::wstring& sql, const std::string& query) -> FilterNode {
  FilterNode node{FilterNode::Type::RawSQL, FilterOp::AND, {}, std::nullopt, sql};
  node.raw_binds_ = {query};
  return node;
}

// Phase S7 step 3: the BM25 alternative is a semi-join on the scored AI documents. It selects
// the same files as the clause it replaced, which called the macro for each library row.
TEST_F(LibrarySearchRecallTest, Bm25SemiJoinMatchesTheSameFilesAsThePerRowClause) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  const auto              specs    = RecallLibrarySpecs();
  const auto              file_ids = builder.AddFiles(specs);
  ASSERT_EQ(file_ids.size(), specs.size());

  auto&      ai = project.GetStorage()->GetAiStore();
  const auto understand = [&](size_t index, const std::string& task_id, const std::string& caption,
                              std::vector<std::string> tags) {
    AiDescription description;
    description.file_id_     = file_ids[index];
    description.task_id_     = task_id;
    description.provider_id_ = "test_provider";
    description.model_id_    = "test_model";
    description.caption_     = caption;
    description.scene_       = "outdoor";
    description.SetTags(std::move(tags));
    ASSERT_TRUE(ai.UpsertUnderstanding(description));
  };
  understand(0, "describe", "Red lighthouse on a rocky cliff", {"lighthouse", "coast"});
  understand(1, "describe", "Lighthouses along the coast at dusk", {"seascape"});
  understand(2, "describe", "Mountain village in morning fog", {"village", "mountain"});
  understand(3, "describe", "Glacier lagoon with floating ice", {"ice"});
  understand(3, "describe_v2", "Icebergs near a black sand beach", {"beach", "coast"});
  understand(6, "describe", "Temple garden with maple trees", {"garden"});
  ASSERT_TRUE(ai.HasUnderstandingFtsIndex()) << "the test runtime ships the DuckDB fts extension";

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);
  const std::wstring  per_row_sql =
      L"(fts_main_AiImageFtsDocument.match_bm25(e.id, ?) IS NOT NULL)";
  const std::wstring semi_join_sql =
      L"e.id IN (SELECT file_id FROM (SELECT file_id, "
      L"fts_main_AiImageFtsDocument.match_bm25(file_id, ?) AS score FROM AiImageFtsDocument) "
      L"WHERE score IS NOT NULL)";

  const std::vector<std::string> queries = {
      "lighthouse", "lighthouses", "coast",        "coast dusk", "fog", "mountain village",
      "icebergs",   "outdoor",     "maple garden", "red cliff",  "desert"};
  size_t queries_with_matches = 0;
  for (const auto& query : queries) {
    const auto per_row   = FilteredFileNames(filter_service, folder_id,
                                             RawFilter(per_row_sql, query));
    const auto semi_join = FilteredFileNames(filter_service, folder_id,
                                             RawFilter(semi_join_sql, query));
    EXPECT_EQ(semi_join, per_row) << "query `" << query << "`";
    if (!per_row.empty()) {
      ++queries_with_matches;
    }
  }
  // Every query but `desert` scores at least one document; `outdoor` (the scene of every
  // understanding) scores all five files that have one.
  EXPECT_EQ(queries_with_matches, queries.size() - 1);
  EXPECT_EQ(FilteredFileNames(filter_service, folder_id, RawFilter(semi_join_sql, "outdoor")).size(),
            5u);

  // The production WHERE uses the semi-join. `lighthouses` matches file 1 by its folded
  // caption and file 0 only through BM25 stemming (its text has `lighthouse`, not the plural).
  const auto where = filter_service.BuildFuzzySearchWhere(L"lighthouses", kAllSearchFields);
  ASSERT_TRUE(where.has_value() && where->raw_sql_.has_value());
  EXPECT_NE(where->raw_sql_->find(semi_join_sql.substr(0, semi_join_sql.find(L'?'))),
            std::wstring::npos);
  EXPECT_EQ(where->raw_sql_->find(L"match_bm25(e.id"), std::wstring::npos);
  EXPECT_EQ(SearchFileNames(filter_service, folder_id, L"lighthouses"),
            (std::set<std::string>{"P2635860.RW2", "P2635861.RW2"}));
}

// ── Stats from one match set (Phase S8) ──────────────────────────────────────────────────

/// Buckets of one query from the stats statements that ElementStore::BuildFolderStats ran
/// before Phase S8, one statement for each bucket kind, each evaluating the filter again.
auto SeparateStatementBuckets(duckdb_connection conn, const std::string& sql,
                              const duckorm::SqlFragment& binds) -> std::vector<StatsBucket> {
  std::vector<StatsBucket> buckets;
  duckdb_result            result;
  if (duckorm::execute_query(conn, sql, binds, &result) != DuckDBSuccess) {
    ADD_FAILURE() << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return buckets;
  }
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
    StatsBucket bucket;
    if (!duckdb_value_is_null(&result, 0, row)) {
      char* label   = duckdb_value_varchar(&result, 0, row);
      bucket.label_ = label;
      duckdb_free(label);
    }
    bucket.count_ = static_cast<int>(duckdb_value_int64(&result, 1, row));
    buckets.push_back(std::move(bucket));
  }
  duckdb_destroy_result(&result);
  return buckets;
}

/// The stats of @p filter computed the way BuildFolderStats did before Phase S8.
auto SeparateStatementStats(ProjectService& project, sl_element_id_t folder_id,
                            const std::optional<FilterNode>& filter, const std::string& model_key)
    -> AlbumStatsView {
  auto           guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto           lock  = guard.Lock();
  const auto     scope = BuildScopedFileQuery(folder_id, CompileFilterPredicate(filter));
  const auto&    from  = scope.from_where_;
  const auto&    binds = scope.binds_;
  AlbumStatsView out;
  const auto     total = SeparateStatementBuckets(
      guard.conn_, std::format("SELECT 'total', COUNT(*) {}", from), binds);
  out.total_photo_count_ = total.empty() ? -1 : total.front().count_;
  out.date_stats_        = SeparateStatementBuckets(
      guard.conn_,
      std::format("SELECT CAST(i.capture_date AS VARCHAR) AS d, COUNT(*) AS c {} "
                                "GROUP BY d ORDER BY d DESC",
                         from),
      binds);
  out.camera_stats_ = SeparateStatementBuckets(
      guard.conn_,
      std::format("SELECT COALESCE(NULLIF(i.camera_model, ''), '(unknown)') AS m, COUNT(*) AS c "
                  "{} GROUP BY m ORDER BY c DESC",
                  from),
      binds);
  out.lens_stats_ = SeparateStatementBuckets(
      guard.conn_,
      std::format("SELECT COALESCE(NULLIF(i.lens, ''), '(unknown)') AS l, COUNT(*) AS c {} "
                  "GROUP BY l ORDER BY c DESC",
                  from),
      binds);
  if (!model_key.empty()) {
    auto label_binds = binds;
    label_binds.binds_.push_back(duckorm::BindValue{model_key});
    out.label_stats_ = SeparateStatementBuckets(
        guard.conn_,
        std::format("WITH scoped AS (SELECT e.id AS file_id {}) "
                    "SELECT sl.label AS l, COUNT(DISTINCT scoped.file_id) AS c FROM scoped "
                    "JOIN SemanticImageLabel sl ON sl.file_id = scoped.file_id "
                    "WHERE sl.model_key = ? AND sl.label IS NOT NULL AND sl.label <> '' "
                    "GROUP BY sl.label ORDER BY c DESC, sl.label",
                    from),
        label_binds);
  }
  out.rating_stats_ = SeparateStatementBuckets(
      guard.conn_,
      std::format("SELECT CAST(i.rating AS VARCHAR) AS r, COUNT(*) AS c {} "
                  "GROUP BY r ORDER BY r DESC",
                  from),
      binds);
  return out;
}

auto BucketText(const std::vector<StatsBucket>& buckets) -> std::string {
  std::string text;
  for (const auto& bucket : buckets) {
    text += std::format("{}={} ", bucket.label_, bucket.count_);
  }
  return text;
}

auto BucketMap(const std::vector<StatsBucket>& buckets) -> std::map<std::string, int> {
  std::map<std::string, int> counts;
  for (const auto& bucket : buckets) {
    counts[bucket.label_] = bucket.count_;
  }
  return counts;
}

/// Cameras and lenses: the separate statements ordered by count only, so equal counts came in
/// any order. The match set orders them by count, then by name; compare the buckets as a map
/// and check that order.
void ExpectSameCountOrderedBuckets(const std::vector<StatsBucket>& match_set,
                                   const std::vector<StatsBucket>& separate,
                                   const std::string&              context) {
  EXPECT_EQ(BucketMap(match_set), BucketMap(separate))
      << context << " match set: " << BucketText(match_set)
      << " separate: " << BucketText(separate);
  EXPECT_EQ(match_set.size(), separate.size()) << context;
  for (size_t i = 1; i < match_set.size(); ++i) {
    const auto& before = match_set[i - 1];
    const auto& after  = match_set[i];
    EXPECT_TRUE(before.count_ > after.count_ ||
                (before.count_ == after.count_ && before.label_ < after.label_))
        << context << " order: " << BucketText(match_set);
  }
}

void ExpectSameStats(const AlbumStatsView& match_set, const AlbumStatsView& separate,
                     const std::string& context) {
  EXPECT_EQ(match_set.total_photo_count_, separate.total_photo_count_) << context;
  EXPECT_EQ(BucketText(match_set.date_stats_), BucketText(separate.date_stats_))
      << context << " date";
  EXPECT_EQ(BucketText(match_set.rating_stats_), BucketText(separate.rating_stats_))
      << context << " rating";
  EXPECT_EQ(BucketText(match_set.label_stats_), BucketText(separate.label_stats_))
      << context << " label";
  ExpectSameCountOrderedBuckets(match_set.camera_stats_, separate.camera_stats_,
                                context + " camera");
  ExpectSameCountOrderedBuckets(match_set.lens_stats_, separate.lens_stats_, context + " lens");
}

void RegisterSemanticModel(Storage& storage, const std::string& model_key, bool active) {
  std::string error;
  ASSERT_TRUE(storage.GetSemanticModelRegistry().UpsertModel(
      SemanticModelRecord{.model_key_     = model_key,
                          .model_id_      = "mobileclip-test",
                          .revision_      = "test-rev",
                          .embedding_dim_ = kSemanticEmbeddingDim,
                          .image_size_    = 256,
                          .active_        = active},
      &error))
      << error;
}

void StoreSemanticLabel(Storage& storage, const SearchResultRow& file, size_t embedding_index,
                        const std::string& model_key, const std::string& label) {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(embedding_index) = 1.0F;
  SemanticImageLabelRecord record{
      .file_id_ = file.file_id_, .model_key_ = model_key, .label_ = label, .score_ = 0.9};
  std::string error;
  ASSERT_TRUE(storage.GetSemanticEmbeddingStore().UpsertImageEmbeddingWithLabel(
      SemanticImageEmbeddingRecord{.file_id_   = file.file_id_,
                                   .image_id_  = file.image_id_,
                                   .model_key_ = model_key,
                                   .embedding_ = std::move(embedding)},
      &record, &error))
      << error;
}

// Phase S8: the stats read from the match set table equal, bucket for bucket, the stats of the
// separate statements they replaced, for a search only, a stats filter only, and both.
TEST_F(LibrarySearchRecallTest, StatsFromMatchSetEqualSeparateStatsQueries) {
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  auto                    specs = RecallLibrarySpecs();
  // Ratings, an unknown camera, an unknown lens, and an unknown date, so every bucket kind
  // has more than one bucket and the unknown buckets are covered.
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].rating_ = static_cast<int>(i % 4);
  }
  specs[5].model_     = "";
  specs[6].lens_      = "";
  specs[7].date_time_ = "";
  const auto file_ids = builder.AddFiles(specs);
  ASSERT_EQ(file_ids.size(), specs.size());

  // Semantic labels of the active model, and one label of an inactive model.
  auto&             storage   = *project.GetStorage();
  const std::string model_key = "match-set-test-model";
  RegisterSemanticModel(storage, "other-model", false);
  RegisterSemanticModel(storage, model_key, true);
  ASSERT_EQ(storage.GetSemanticModelRegistry().ActiveModelKey(), model_key);
  const auto files = storage.GetElementStore().ListSearchResultRows(file_ids);
  ASSERT_EQ(files.size(), file_ids.size());
  StoreSemanticLabel(storage, files[0], 0, model_key, "landscape");
  StoreSemanticLabel(storage, files[1], 1, model_key, "landscape");
  StoreSemanticLabel(storage, files[2], 2, model_key, "portrait");
  StoreSemanticLabel(storage, files[3], 3, model_key, "landscape");
  StoreSemanticLabel(storage, files[4], 4, "other-model", "portrait");

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);

  std::vector<std::pair<std::string, std::optional<FilterNode>>> filters;
  filters.emplace_back("no filter", std::nullopt);
  for (const auto* query : {L"nikon", L"2026-06-07", L"dng", L"shangrila", L"jpg"}) {
    filters.emplace_back("search " + conv::ToBytes(query),
                         filter_service.BuildFuzzySearchWhere(query, kAllSearchFields));
  }
  const auto rating_one   = sleeve_filter::BuildRatingBucketFilter(L"1");
  const auto nikon_camera = sleeve_filter::BuildCameraModelBucketFilter(L"NIKON Z 7");
  filters.emplace_back("stats filter rating 1", rating_one);
  filters.emplace_back("stats filter camera", nikon_camera);
  filters.emplace_back("stats filter unknown date", sleeve_filter::BuildCaptureDateUnknownFilter());
  filters.emplace_back("search nikon and rating 1",
                       MergeFilterNodes(rating_one, filter_service.BuildFuzzySearchWhere(
                                                        L"nikon", kAllSearchFields)));
  filters.emplace_back("search 2026 and camera",
                       MergeFilterNodes(nikon_camera, filter_service.BuildFuzzySearchWhere(
                                                          L"2026", kAllSearchFields)));

  for (const auto& [context, filter] : filters) {
    const auto separate = SeparateStatementStats(project, folder_id, filter, model_key);
    ExpectSameStats(filter_service.BuildFolderStats(folder_id, filter), separate, context);

    const auto page_and_stats =
        filter_service.ListSearchResultPageWithStats(folder_id, filter, 0, 1000);
    ExpectSameStats(page_and_stats.stats_, separate, context + " (apply)");
    EXPECT_EQ(page_and_stats.page_.total_, static_cast<size_t>(separate.total_photo_count_))
        << context;
    const auto page = filter_service.ListSearchResultPage(folder_id, filter, 0, 1000);
    ASSERT_EQ(page_and_stats.page_.rows_.size(), page.rows_.size()) << context;
    for (size_t i = 0; i < page.rows_.size(); ++i) {
      const auto& actual   = page_and_stats.page_.rows_[i];
      const auto& expected = page.rows_[i];
      EXPECT_EQ(actual.file_id_, expected.file_id_) << context;
      EXPECT_EQ(actual.image_id_, expected.image_id_) << context;
      EXPECT_EQ(actual.file_name_, expected.file_name_) << context;
      EXPECT_EQ(actual.camera_model_, expected.camera_model_) << context;
      EXPECT_EQ(actual.lens_, expected.lens_) << context;
      EXPECT_EQ(actual.capture_date_, expected.capture_date_) << context;
      EXPECT_EQ(actual.rating_, expected.rating_) << context;
    }
  }

  // The unfiltered library has two buckets or more of every kind, and the unknown buckets.
  const auto all = filter_service.BuildFolderStats(folder_id, std::nullopt);
  EXPECT_EQ(all.total_photo_count_, static_cast<int>(specs.size()));
  EXPECT_EQ(BucketMap(all.label_stats_),
            (std::map<std::string, int>{{"landscape", 3}, {"portrait", 1}}));
  EXPECT_EQ(BucketMap(all.camera_stats_).count("(unknown)"), 1u);
  EXPECT_EQ(BucketMap(all.lens_stats_).count("(unknown)"), 1u);
  ASSERT_FALSE(all.date_stats_.empty());
  EXPECT_EQ(all.date_stats_.back().label_, "") << "the unknown date bucket is last";
  EXPECT_EQ(BucketMap(all.rating_stats_).size(), 4u);

  // A page of the apply read leaves out the other rows; the total is the whole match set.
  const auto second_page =
      filter_service.ListSearchResultPageWithStats(folder_id, std::nullopt, 2, 3);
  EXPECT_EQ(second_page.page_.total_, specs.size());
  ASSERT_EQ(second_page.page_.rows_.size(), 3u);
  EXPECT_EQ(second_page.page_.rows_.front().file_id_, file_ids[2]);
  const auto past_end =
      filter_service.ListSearchResultPageWithStats(folder_id, std::nullopt, 50, 3);
  EXPECT_TRUE(past_end.page_.rows_.empty());
  EXPECT_EQ(past_end.page_.total_, specs.size());
}

// ── Local Nikon folder (disabled by default) ──────────────────────────────────────────────
//
// alcedo_studio/tests/resources/sample_images/raw/camera/nikon is local-only (git-ignored,
// about 1.3 GB). Run with --gtest_also_run_disabled_tests.

auto NikonFixtureDir() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "raw" / "camera" / "nikon";
}

// Mirror the folder import file list: every regular file, recursively, with no extension check.
auto CollectAllRegularFiles(const std::filesystem::path& dir) -> std::vector<image_path_t> {
  std::vector<image_path_t> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

struct NikonImportOutcome {
  ImportResult result_{};
};

auto ImportFolderToLibraryRoot(ProjectService& project, const std::vector<image_path_t>& paths)
    -> NikonImportOutcome {
  auto import_service = std::make_unique<ImportServiceImpl>(project.GetSleeveService(),
                                                            project.GetImagePoolService());
  auto job            = std::make_shared<ImportJob>();
  std::promise<ImportResult> finished;
  auto                       finished_future = finished.get_future();
  job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
  job               = import_service->ImportToFolder(paths, L"", {}, job);
  EXPECT_NE(job, nullptr);
  NikonImportOutcome outcome;
  outcome.result_ = finished_future.get();
  import_service->SyncImports(job->import_log_->Snapshot(), L"");
  return outcome;
}

TEST_F(LibrarySearchRecallTest, DISABLED_NikonFolderImportsRawOnlyAndSearchFindsFileNames) {
  const auto fixture_dir = NikonFixtureDir();
  if (!std::filesystem::exists(fixture_dir)) {
    GTEST_SKIP() << "Local Nikon fixture folder is missing: " << fixture_dir.string();
  }
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);

  const auto paths = CollectAllRegularFiles(fixture_dir);
  ASSERT_EQ(paths.size(), 45u) << "The fixture folder must hold 38 RAW and 7 non-RAW files";

  ProjectService project(db_path_, meta_path_);
  const auto     outcome = ImportFolderToLibraryRoot(project, paths);
  EXPECT_EQ(outcome.result_.requested_, 45u);
  EXPECT_EQ(outcome.result_.imported_, 38u);
  EXPECT_EQ(outcome.result_.failed_, 7u);

  EXPECT_EQ(CountTableRows(project, "FileImage"), 38);
  // Phase S1: SyncImports removes the Image of each failed import, so no orphan rows remain.
  EXPECT_EQ(CountTableRows(project, "Image"), 38);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          folder_id = LibraryRootFolderId(project);
  const std::string   d3x = "nikon_d3x_01.nef", d810 = "Nikon-D810-raw00011.nef",
                    z8_2230 = "DSC_2230.NEF", z7_dng = "DSC_0431_dng.dng";
  const std::set<std::string> z8_files  = {"5761824892.nef", "DSC_1211.dng", "DSC_2230.NEF",
                                           "nikon-z8-raw-00004.nef"};
  const std::set<std::string> dng_files = {"z6iii.dng", "DSC_0431_dng.dng", "DSC_1211.dng"};

  // The folder holds only NEF and DNG files, so these cases check the named file is found
  // (recall). Precision for these names is covered by the synthetic table above.
  for (const auto* query : {L"nikon d3x 01", L"d3x_01", L"nikond3x01"}) {
    EXPECT_TRUE(SearchFileNames(filter_service, folder_id, query).contains(d3x))
        << conv::ToBytes(query);
  }
  for (const auto* query : {L"d810 raw 11", L"raw00011", L"00011"}) {
    EXPECT_TRUE(SearchFileNames(filter_service, folder_id, query).contains(d810))
        << conv::ToBytes(query);
  }
  for (const auto* query : {L"dsc 2230", L"DSC2230", L"2230"}) {
    EXPECT_TRUE(SearchFileNames(filter_service, folder_id, query).contains(z8_2230))
        << conv::ToBytes(query);
  }
  EXPECT_TRUE(SearchFileNames(filter_service, folder_id, L"0431 dng").contains(z7_dng));

  // Exact sets (Phase S4): `z8` matches the z8 folder name and the Z 8 camera model only;
  // `dng` is a file kind term and matches only the DNG files.
  const auto  z8_result  = SearchFileNames(filter_service, folder_id, L"z8");
  const auto  dng_result = SearchFileNames(filter_service, folder_id, L"dng");
  std::string z8_text, dng_text;
  for (const auto& name : z8_result) z8_text += name + " ";
  for (const auto& name : dng_result) dng_text += name + " ";
  RecordProperty("z8_result", z8_text);
  RecordProperty("dng_result", dng_text);
  EXPECT_EQ(z8_result, z8_files) << "z8 result: " << z8_text;
  EXPECT_EQ(dng_result, dng_files) << "dng result: " << dng_text;
}

}  // namespace
}  // namespace alcedo
