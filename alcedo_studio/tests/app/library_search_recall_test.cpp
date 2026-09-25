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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/store/ai/ai_store.hpp"
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
