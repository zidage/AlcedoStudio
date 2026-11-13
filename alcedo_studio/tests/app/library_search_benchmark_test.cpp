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
//   apply   = BuildFuzzySearchWhere + ListSearchResultPageWithStats(page of 120)
//                                                                (grid page + stats panel)
// Since Phase S8 the apply read evaluates the search predicate once for the page, the total,
// and the stats.
//
// A third benchmark measures the natural-language relevance read
// (global_search_redesign_plan.md, Phase GS1): SemanticVectorSearch::SearchImageEmbeddings
// over one folder of 20 000 synthetic 512-dimensional embeddings. Target: p50 at or below 80 ms
// in a release build. A fourth measures the same read for a folder of 100 files in projects of
// 1 000 and 100 000 embeddings (issue 427): the p50 must not grow with the project size.
//
// Environment variables:
//   ALCEDO_SEARCH_BENCH_REPEAT   runs per query (default 3)
//   ALCEDO_SEARCH_BENCH_PROJECT  path to a packed .alcd project for the real-project test

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "library_search_test_support.hpp"
#include "process_memory_test_support.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"
#include "utils/clock/time_provider.hpp"
#include "../support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"

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
      const auto applied =
          service.ListSearchResultPageWithStats(folder_id, where, 0, kApplyPageSize);
      latency.apply_ms_.push_back(ElapsedMs(start));
      EXPECT_EQ(applied.page_.total_, latency.match_count_) << conv::ToBytes(query);
      EXPECT_EQ(static_cast<size_t>(applied.stats_.total_photo_count_), latency.match_count_)
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
    auto guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
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

/// Median time of `repeat` WHERE builds. Since Phase S7 the build runs no SQL.
auto MedianWhereBuildMs(const SleeveFilterService& service, int repeat) -> double {
  std::vector<double> samples;
  for (int run = 0; run < repeat; ++run) {
    const auto start = Clock::now();
    const auto where = service.BuildFuzzySearchWhere(L"dsc", kAllSearchFields);
    samples.push_back(ElapsedMs(start));
    EXPECT_TRUE(where.has_value());
  }
  return Percentile(samples, 50);
}

void ReportFixedCosts(const std::string& library_label, ProjectService& project,
                      const SleeveFilterService& service) {
  // The AI FTS index probe that BuildFuzzySearchWhere ran on every call before Phase S7
  // (AiStore::HasUnderstandingFtsIndex now reads the state recorded at the last index build).
  constexpr const char* kCatalogProbe =
      "SELECT COUNT(*) FROM duckdb_functions() WHERE schema_name = "
      "'fts_main_AiImageFtsDocument' AND function_name = 'match_bm25'";
  const auto line = std::format(
      "  private memory {:.0f} MiB, SELECT 1 {:.3f} ms, catalog probe {:.1f} ms, "
      "WHERE build {:.3f} ms (p50 of 20)",
      process_memory_test::Mebibytes(process_memory_test::PrivateBytes()),
      MedianStatementMs(project, "SELECT 1", 20), MedianStatementMs(project, kCatalogProbe, 20),
      MedianWhereBuildMs(service, 20));
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
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    TimeProvider::Refresh();
    work_dir_ = temp_dir_.Path();
  }

  void RunSyntheticBenchmark(size_t count, size_t dng_per_mille) {
    ProjectService project(work_dir_ / "bench.db", work_dir_ / "bench.json");
    const auto     build_start = Clock::now();
    const auto     specs       = BenchmarkLibrarySpecs(count, dng_per_mille);
    ASSERT_EQ(SyntheticLibraryBuilder(project).AddFiles(specs).size(), count);
    std::cout << std::format("\nBuilt {} synthetic files in {:.1f} s\n", count,
                             ElapsedMs(build_start) / 1000.0);

    const auto label = std::format("synthetic {} files, {} DNG per 1000", count, dng_per_mille);
    SleeveFilterService service(project.GetStorage());
    ReportFixedCosts(label, project, service);

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

/// A deterministic unit-length vector of the 512-dimensional model; @p salt selects the vector.
auto SyntheticUnitVector(uint32_t salt) -> std::vector<float> {
  std::vector<float> values(kSemanticEmbeddingDim);
  uint32_t           state = 2166136261U ^ salt;
  double             norm  = 0.0;
  for (auto& value : values) {
    state = state * 1664525U + 1013904223U;
    value = static_cast<float>(state >> 8U) / static_cast<float>(1U << 24U) - 0.5F;
    norm += static_cast<double>(value) * value;
  }
  for (auto& value : values) {
    value = static_cast<float>(value / std::sqrt(norm));
  }
  return values;
}

constexpr const char* kRelevanceModelKey = "relevance-benchmark";

/// The SQL expression of a pseudo-random 512-value embedding for the file id @p file_id_sql.
auto                  PseudoRandomEmbeddingSql(const std::string& file_id_sql) -> std::string {
  return std::format(
      "CAST(list_transform(range({0}), k -> CAST(hash({1} * {0} + k) % 2001 AS FLOAT) / 1000.0 - "
                       "1.0) AS FLOAT[{0}])",
      kSemanticEmbeddingDim, file_id_sql);
}

void ExecuteBenchmarkSql(Storage& storage, const std::string& sql) {
  auto          guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  ASSERT_EQ(duckdb_query(guard.conn_, sql.c_str(), &result), DuckDBSuccess)
      << duckdb_result_error(&result);
  duckdb_destroy_result(&result);
}

/// Registers the benchmark model and writes a pseudo-random ready embedding for every file
/// (FileImage row) that has no embedding yet.
void WriteEmbeddingsForLibraryFiles(Storage& storage) {
  std::string error;
  ASSERT_TRUE(storage.GetSemanticModelRegistry().UpsertModel(
      SemanticModelRecord{.model_key_     = kRelevanceModelKey,
                          .model_id_      = "relevance-benchmark",
                          .revision_      = "bench",
                          .embedding_dim_ = kSemanticEmbeddingDim,
                          .image_size_    = 256},
      &error))
      << error;
  ExecuteBenchmarkSql(
      storage,
      std::format("INSERT INTO SemanticImageEmbedding (file_id, image_id, model_key, embedding, "
                  "embedding_dim, thumbnail_resolution, status, error) "
                  "SELECT fi.file_id, fi.image_id, '{}', {}, {}, 256, 'ready', NULL "
                  "FROM FileImage fi WHERE NOT EXISTS (SELECT 1 FROM SemanticImageEmbedding se "
                  "WHERE se.file_id = fi.file_id)",
                  kRelevanceModelKey, PseudoRandomEmbeddingSql("fi.file_id"),
                  kSemanticEmbeddingDim));
}

/**
 * Writes @p count files of another folder with a ready embedding each, by SQL: Element, Image,
 * FileImage, and FolderContent rows and an embedding row. Only the table sizes matter to the
 * relevance read of the library root, so the files have no Sleeve objects. Their ids start at
 * @p first_id.
 */
void WriteOtherFolderFiles(Storage& storage, int64_t first_id, size_t count) {
  constexpr int64_t kOtherFolderId = 999999999;
  ExecuteBenchmarkSql(
      storage,
      std::format(
          "INSERT INTO Element SELECT {0} + r, {3}, 'other_' || r, now(), now(), 1 "
          "FROM range({1}) t(r); "
          "INSERT INTO Image (id, file_name) SELECT {0} + r, 'other_' || r FROM range({1}) t(r); "
          "INSERT INTO FileImage SELECT {0} + r, {0} + r FROM range({1}) t(r); "
          "INSERT INTO FolderContent SELECT {2}, {0} + r FROM range({1}) t(r); "
          "INSERT INTO SemanticImageEmbedding (file_id, image_id, model_key, embedding, "
          "embedding_dim, thumbnail_resolution, status, error) "
          "SELECT {0} + r, {0} + r, '{4}', {5}, {6}, 256, 'ready', NULL FROM range({1}) t(r)",
          first_id, count, kOtherFolderId, static_cast<uint32_t>(ElementType::FILE),
          kRelevanceModelKey, PseudoRandomEmbeddingSql(std::format("({} + r)", first_id)),
          kSemanticEmbeddingDim));
}

struct RelevanceReadLatency {
  size_t kept_count_ = 0;
  double p50_ms_     = 0.0;
  double p95_ms_     = 0.0;
  int    runs_       = 0;
};

/// Times SearchImageEmbeddings (page of 120) on the folder @p folder_id of @p project.
auto MeasureRelevanceRead(ProjectService& project, sl_element_id_t folder_id)
    -> RelevanceReadLatency {
  constexpr size_t             kPageSize = 120;
  const SemanticQueryEmbedding embedding{.model_key_ = kRelevanceModelKey,
                                         .dimension_ = kSemanticEmbeddingDim,
                                         .query_     = SyntheticUnitVector(1),
                                         .neutral_   = SyntheticUnitVector(2)};
  const auto&                  search = project.GetStorage()->GetSemanticVectorSearch();
  std::vector<double>          samples;
  RelevanceReadLatency         latency;
  latency.runs_ = std::max(RepeatCount(), 20);
  for (int run = 0; run < latency.runs_; ++run) {
    std::string error;
    const auto  start = Clock::now();
    const auto  page  = search.SearchImageEmbeddings(folder_id, embedding, 0, kPageSize, &error);
    samples.push_back(ElapsedMs(start));
    EXPECT_TRUE(error.empty()) << error;
    latency.kept_count_ = page.total_;
  }
  latency.p50_ms_ = Percentile(samples, 50);
  latency.p95_ms_ = Percentile(samples, 95);
  return latency;
}

TEST_F(LibrarySearchBenchmarkTest, DISABLED_RelevanceSelectOver20000Embeddings) {
  constexpr size_t kFileCount = 20000;
  ProjectService   project(work_dir_ / "relevance.db", work_dir_ / "relevance.json");
  ASSERT_EQ(SyntheticLibraryBuilder(project).AddFiles(BenchmarkLibrarySpecs(kFileCount, 0)).size(),
            kFileCount);
  auto& storage = *project.GetStorage();
  WriteEmbeddingsForLibraryFiles(storage);
  ASSERT_EQ(storage.GetSemanticEmbeddingStore().CountImageEmbeddings(kRelevanceModelKey),
            kFileCount);

  const auto latency = MeasureRelevanceRead(project, LibraryRootFolderId(project));
  const auto line =
      std::format("  {} embeddings, {} kept, p50 {:.1f} ms, p95 {:.1f} ms ({} runs)", kFileCount,
                  latency.kept_count_, latency.p50_ms_, latency.p95_ms_, latency.runs_);
  std::cout << "\n[natural-language relevance read]\n" << line << "\n";
  std::cout.flush();
  ::testing::Test::RecordProperty("relevance read", line);
  EXPECT_LE(latency.p50_ms_, 80.0);
}

/**
 * A folder of 100 files in a project of 1 000 and of 100 000 embeddings. The folder files are
 * imported together, so their ids are contiguous, and half of the other embedding rows are
 * written before the folder rows and half after them. The relevance read multiplies only the
 * folder rows and skips the vectors outside their row range, so the p50 of the large project
 * must stay within twice the p50 of the small project.
 *
 * A third project of 100 000 embeddings adds one folder file that is written after every other
 * file, so the folder rows span the whole table. Its p50 is reported and not checked: DuckDB 1.2
 * reads the vectors of every row in the range of the folder rows.
 */
TEST_F(LibrarySearchBenchmarkTest, DISABLED_RelevanceSelectOfFolderDoesNotGrowWithProjectSize) {
  constexpr size_t          kFolderFileCount = 100;
  constexpr sl_element_id_t kFolderId        = 777;
  constexpr int64_t         kOtherFirstId    = 1000000;
  constexpr int64_t         kLastFileId      = 9000000;
  const auto measure = [&](size_t project_embedding_count, bool last_file_in_folder) {
    const auto     name = std::format("relevance_{}_{}", project_embedding_count,
                                  last_file_in_folder ? "spanning" : "contiguous");
    ProjectService project(work_dir_ / (name + ".db"), work_dir_ / (name + ".json"));
    EXPECT_EQ(SyntheticLibraryBuilder(project)
                  .AddFiles(BenchmarkLibrarySpecs(kFolderFileCount, 0))
                  .size(),
              kFolderFileCount);
    auto& storage = *project.GetStorage();
    // The library files are also the direct children of the folder.
    ExecuteBenchmarkSql(storage, std::format("INSERT INTO FolderContent SELECT {}, file_id FROM "
                                             "FileImage",
                                             kFolderId));
    const auto other_count = project_embedding_count - kFolderFileCount;
    const auto before      = other_count / 2;
    WriteOtherFolderFiles(storage, kOtherFirstId, before);
    WriteEmbeddingsForLibraryFiles(storage);
    WriteOtherFolderFiles(storage, kOtherFirstId + static_cast<int64_t>(before),
                          other_count - before);
    size_t expected_count = project_embedding_count;
    if (last_file_in_folder) {
      WriteOtherFolderFiles(storage, kLastFileId, 1);
      ExecuteBenchmarkSql(storage, std::format("INSERT INTO FolderContent VALUES ({}, {})",
                                               kFolderId, kLastFileId));
      ++expected_count;
    }
    EXPECT_EQ(storage.GetSemanticEmbeddingStore().CountImageEmbeddings(kRelevanceModelKey),
              expected_count);
    return MeasureRelevanceRead(project, kFolderId);
  };
  const auto report = [](const std::string& label, const RelevanceReadLatency& latency) {
    const auto line =
        std::format("  {}: {} kept, p50 {:.1f} ms, p95 {:.1f} ms ({} runs)", label,
                    latency.kept_count_, latency.p50_ms_, latency.p95_ms_, latency.runs_);
    std::cout << "\n[natural-language relevance read of a folder]\n" << line << "\n";
    ::testing::Test::RecordProperty(label, line);
  };

  const auto small_project    = measure(1000, false);
  const auto large_project    = measure(100000, false);
  const auto spanning_project = measure(100000, true);
  report("folder of 100 files, 1000 project embeddings", small_project);
  report("folder of 100 files, 100000 project embeddings", large_project);
  report("folder of 101 files spanning 100001 project embeddings", spanning_project);
  std::cout.flush();
  EXPECT_EQ(large_project.kept_count_, small_project.kept_count_);
  EXPECT_LE(large_project.p50_ms_, 2.0 * small_project.p50_ms_);
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
