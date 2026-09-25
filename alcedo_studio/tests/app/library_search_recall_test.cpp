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

#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
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
  constexpr auto defect = CaseState::kKnownDefect;
  constexpr auto passes = CaseState::kPasses;
  return {
      // Dates: month-day forms match June 7 of any year; "67" also matches IMG_0067.
      {L"6.7", {f1, f2, f4, f8}, defect},
      {L"6/7", {f1, f2, f4, f8}, defect},
      {L"0607", {f1, f2, f4}, passes},  // Phase S2: no DNG profile numbers in metadata
      {L"6月7日", {f1, f2, f4}, defect},
      {L"June 7", {f1, f2, f4}, defect},
      {L"2026.6", {f1, f2, f3}, passes},
      {L"2026-06-07", {f1, f2}, passes},
      {L"20260607", {f1, f2}, passes},
      // File kinds.
      {L"jpg", {}, passes},
      {L"dng", {f4, f5}, passes},  // Phase S3: no metadata dump in the search text
      {L"rw2", {f1, f2, f3, f9}, passes},
      // Every file is RAW. Before Phase S3 this matched only through the metadata JSON dump
      // (the `RawRuntimeColorContext` key); now only `raw00011` matches until the Phase S4
      // file kind term.
      {L"raw", {f1, f2, f3, f4, f5, f6, f7, f8, f9}, defect},
      // Capture parameters.
      {L"iso800", {f1}, defect},
      {L"f2.8", {f2}, defect},
      {L"35mm", {f2}, defect},
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

  // Exact sets. Known defects until Phase S3/S4: `z8` also matches files whose metadata
  // dump contains "z8"; `dng` matches every file whose metadata dump contains a DNG key.
  const auto  z8_result  = SearchFileNames(filter_service, folder_id, L"z8");
  const auto  dng_result = SearchFileNames(filter_service, folder_id, L"dng");
  std::string z8_text, dng_text;
  for (const auto& name : z8_result) z8_text += name + " ";
  for (const auto& name : dng_result) dng_text += name + " ";
  RecordProperty("z8_result", z8_text);
  RecordProperty("dng_result", dng_text);
  EXPECT_TRUE(std::includes(z8_result.begin(), z8_result.end(), z8_files.begin(), z8_files.end()))
      << "z8 result: " << z8_text;
  EXPECT_TRUE(
      std::includes(dng_result.begin(), dng_result.end(), dng_files.begin(), dng_files.end()))
      << "dng result: " << dng_text;
}

}  // namespace
}  // namespace alcedo
