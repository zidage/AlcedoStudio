//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Latency benchmark for the library fuzzy search (library_search_and_project_size_plan.md,
// Phase S0). The tests are disabled by default because the current search takes seconds for
// each query. Run them with --gtest_also_run_disabled_tests.
//
// Two measured paths, both through SleeveFilterService (the service the UI calls). Since
// Phase S5 both run on the search worker, and the page statement also returns the total:
//   preview = SearchFolderPage(page of 50)                       (one search dialog keystroke)
//   apply   = BuildFuzzySearchWhere + ListSearchResultPage(page of 120)
//             + BuildFolderStats(search filter)                  (grid page + stats panel)
//
// Environment variables:
//   ALCEDO_SEARCH_BENCH_REPEAT   runs per query (default 3)
//   ALCEDO_SEARCH_BENCH_PROJECT  path to a packed .alcd project for the real-project test

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
#include "process_memory_test_support.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using library_search_test::LibraryRootFolderId;
using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;
using Clock                       = std::chrono::steady_clock;

constexpr size_t kPreviewPageSize = 50;
constexpr size_t kApplyPageSize   = 120;  // kSearchMetadataPageSize in library_module.cpp

// Queries measured on demo.alcd before this plan: a miss, a date, a file-name miss, a short
// numeric query, and a common file-name prefix.
const std::vector<std::wstring> kBenchmarkQueries = {L"jpg", L"2026-06-07", L"P1000123", L"6.7",
                                                     L"dsc"};

auto                            RepeatCount() -> int {
  if (const char* value = std::getenv("ALCEDO_SEARCH_BENCH_REPEAT")) {
    return std::max(1, std::atoi(value));
  }
  return 3;
}

/// Nearest-rank percentile of `samples` (milliseconds).
auto Percentile(std::vector<double> samples, double percentile) -> double {
  std::sort(samples.begin(), samples.end());
  const auto rank =
      static_cast<size_t>(std::ceil(percentile / 100.0 * static_cast<double>(samples.size())));
  return samples[std::clamp<size_t>(rank, 1, samples.size()) - 1];
}

auto ElapsedMs(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Synthetic library of `count` files. `dng_per_mille` files in each 1000 are DNG files with
/// the large embedded profile; the others are RW2 and NEF files with a small RAW color context.
auto BenchmarkLibrarySpecs(size_t count, size_t dng_per_mille) -> std::vector<SyntheticImageSpec> {
  using namespace std::chrono;
  constexpr float                 kApertures[] = {1.8f, 2.8f, 4.0f, 5.6f, 8.0f};
  constexpr uint64_t              kIsos[]      = {100, 200, 400, 800, 1600, 3200};
  constexpr float                 kFocals[]    = {24.0f, 35.0f, 50.0f, 85.0f};
  const sys_days                  first_day    = 2024y / January / 1d;

  std::vector<SyntheticImageSpec> specs;
  specs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const year_month_day day{first_day + days{static_cast<int>(i % 900)}};
    const auto           date =
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(day.year()),
                    static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()),
                    8 + i % 12, i % 60, (i * 7) % 60);
    const auto         folder = std::format(L"D:/photos/trip_{:03}/", i / 250);

    SyntheticImageSpec spec;
    spec.date_time_ = date;
    spec.iso_       = kIsos[i % std::size(kIsos)];
    spec.aperture_  = kApertures[i % std::size(kApertures)];
    spec.focal_     = kFocals[i % std::size(kFocals)];
    spec.rating_    = static_cast<int>(i % 6);
    if (i % 1000 < dng_per_mille) {
      spec.file_name_       = std::format(L"DSC_{:06}.dng", i);
      spec.make_            = "NIKON CORPORATION";
      spec.model_           = "NIKON Z 7";
      spec.lens_            = "NIKKOR Z 24-70mm f/4 S";
      spec.has_dng_profile_ = true;
    } else if (i % 5 < 3) {
      spec.file_name_ = std::format(L"P{:07}.RW2", 2600000 + i);
      spec.make_      = "Panasonic";
      spec.model_     = "DC-G9M2";
      spec.lens_      = "LUMIX G VARIO 12-60/F3.5-5.6";
    } else {
      spec.file_name_ = std::format(L"DSC_{:06}.NEF", i);
      spec.make_      = "NIKON CORPORATION";
      spec.model_     = "NIKON Z 8";
      spec.lens_      = "NIKKOR Z 70-200mm f/2.8 VR S";
    }
    spec.image_path_ = folder + spec.file_name_;
    specs.push_back(std::move(spec));
  }
  return specs;
}

struct QueryLatency {
  std::wstring        query_;
  size_t              match_count_ = 0;
  std::vector<double> preview_ms_;
  std::vector<double> apply_ms_;
};

/// Run the preview and apply paths `repeat` times for each query and check that the count
/// query and the unpaged search agree (both use the same compiled predicate).
auto MeasureSearchLatency(const SleeveFilterService& service, sl_element_id_t folder_id, int repeat)
    -> std::vector<QueryLatency> {
  std::vector<QueryLatency> results;
  for (const auto& query : kBenchmarkQueries) {
    QueryLatency latency{.query_ = query};
    latency.match_count_ = service.CountSearchResults(folder_id, query, kAllSearchFields);
    EXPECT_EQ(service.SearchFolder(folder_id, query, 0, 0, kAllSearchFields).size(),
              latency.match_count_)
        << conv::ToBytes(query);

    for (int run = 0; run < repeat; ++run) {
      auto       start = Clock::now();
      const auto preview_page =
          service.SearchFolderPage(folder_id, query, 0, kPreviewPageSize, kAllSearchFields);
      latency.preview_ms_.push_back(ElapsedMs(start));
      EXPECT_EQ(preview_page.total_, latency.match_count_) << conv::ToBytes(query);

      start                 = Clock::now();
      const auto where      = service.BuildFuzzySearchWhere(query, kAllSearchFields);
      const auto apply_page = service.ListSearchResultPage(folder_id, where, 0, kApplyPageSize);
      const auto stats      = service.BuildFolderStats(folder_id, where);
      latency.apply_ms_.push_back(ElapsedMs(start));
      EXPECT_EQ(apply_page.total_, latency.match_count_) << conv::ToBytes(query);
      EXPECT_EQ(static_cast<size_t>(stats.total_photo_count_), latency.match_count_)
          << conv::ToBytes(query);
    }
    results.push_back(std::move(latency));
  }
  return results;
}

/// Median time of `repeat` runs of a statement that reads no library data. Since Phase S6 it
/// must cost the same at 1000 and 20 000 files (the retained memory made it 8-70x slower).
auto MedianStatementMs(ProjectService& project, const char* sql, int repeat) -> double {
  std::vector<double> samples;
  for (int run = 0; run < repeat; ++run) {
    auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto          lock  = guard.Lock();
    duckdb_result result;
    const auto    start = Clock::now();
    const auto    state = duckdb_query(guard.conn_, sql, &result);
    samples.push_back(ElapsedMs(start));
    EXPECT_EQ(state, DuckDBSuccess) << sql;
    duckdb_destroy_result(&result);
  }
  return Percentile(samples, 50);
}

void ReportFixedCosts(const std::string& library_label, ProjectService& project) {
  // The AI FTS index probe that BuildFuzzySearchWhere runs (AiStore::HasUnderstandingFtsIndex).
  constexpr const char* kCatalogProbe =
      "SELECT COUNT(*) FROM duckdb_functions() WHERE schema_name = "
      "'fts_main_AiImageFtsDocument' AND function_name = 'match_bm25'";
  const auto line = std::format(
      "  private memory {:.0f} MiB, SELECT 1 {:.3f} ms, catalog probe {:.1f} ms (p50 of 20)",
      process_memory_test::Mebibytes(process_memory_test::PrivateBytes()),
      MedianStatementMs(project, "SELECT 1", 20), MedianStatementMs(project, kCatalogProbe, 20));
  std::cout << "\n[library fixed costs] " << library_label << "\n" << line << "\n";
  ::testing::Test::RecordProperty(library_label + " fixed costs", line);
}

void ReportLatency(const std::string& library_label, const std::vector<QueryLatency>& results) {
  std::cout << "\n[library search latency] " << library_label << "\n"
            << std::format("  {:<12} {:>8} {:>12} {:>12} {:>12} {:>12}\n", "query", "matches",
                           "preview p50", "preview p95", "apply p50", "apply p95");
  for (const auto& r : results) {
    const auto line = std::format("  {:<12} {:>8} {:>10.1f}ms {:>10.1f}ms {:>10.1f}ms {:>10.1f}ms",
                                  conv::ToBytes(r.query_), r.match_count_,
                                  Percentile(r.preview_ms_, 50), Percentile(r.preview_ms_, 95),
                                  Percentile(r.apply_ms_, 50), Percentile(r.apply_ms_, 95));
    std::cout << line << "\n";
    ::testing::Test::RecordProperty(library_label + " " + conv::ToBytes(r.query_), line);
  }
  std::cout.flush();
}

class LibrarySearchBenchmarkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    work_dir_ = std::filesystem::temp_directory_path() / "library_search_benchmark_test";
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
    std::filesystem::create_directories(work_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
  }

  void RunSyntheticBenchmark(size_t count, size_t dng_per_mille) {
    ProjectService project(work_dir_ / "bench.db", work_dir_ / "bench.json");
    const auto     build_start = Clock::now();
    const auto     specs       = BenchmarkLibrarySpecs(count, dng_per_mille);
    ASSERT_EQ(SyntheticLibraryBuilder(project).AddFiles(specs).size(), count);
    std::cout << std::format("\nBuilt {} synthetic files in {:.1f} s\n", count,
                             ElapsedMs(build_start) / 1000.0);

    const auto label = std::format("synthetic {} files, {} DNG per 1000", count, dng_per_mille);
    ReportFixedCosts(label, project);

    SleeveFilterService service(project.GetStorage());
    const auto results = MeasureSearchLatency(service, LibraryRootFolderId(project), RepeatCount());
    ReportLatency(label, results);
  }

  std::filesystem::path work_dir_;
};

// 155 DNG files per 1000 matches demo.alcd (144 DNG in 927 files).
TEST_F(LibrarySearchBenchmarkTest, DISABLED_ReportsSearchLatencyForOneThousandFileLibrary) {
  RunSyntheticBenchmark(1000, 155);
}

// A lower DNG share keeps the temporary database near 200 MB with the current profile storage.
TEST_F(LibrarySearchBenchmarkTest, DISABLED_ReportsSearchLatencyForTwentyThousandFileLibrary) {
  RunSyntheticBenchmark(20000, 20);
}

TEST_F(LibrarySearchBenchmarkTest, DISABLED_ReportsSearchLatencyForPackedProject) {
  const char* packed = std::getenv("ALCEDO_SEARCH_BENCH_PROJECT");
  if (packed == nullptr || *packed == '\0') {
    GTEST_SKIP() << "Set ALCEDO_SEARCH_BENCH_PROJECT to a packed .alcd project";
  }
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(std::filesystem::path(packed), work_dir_,
                                                     QStringLiteral("bench"), &db_path, &meta_path,
                                                     &error))
      << error.toStdString();

  ProjectService      project(db_path, meta_path);
  SleeveFilterService service(project.GetStorage());
  const auto results = MeasureSearchLatency(service, LibraryRootFolderId(project), RepeatCount());
  ReportLatency(std::filesystem::path(packed).filename().string(), results);
}

}  // namespace
}  // namespace alcedo
