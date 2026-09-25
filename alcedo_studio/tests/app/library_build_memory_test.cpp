//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Memory use of library writes (library_search_and_project_size_plan.md, Phase S6).
// The tests read the process private bytes (Windows) around library builds through the
// production image pool and Sleeve services.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "app/project_service.hpp"
#include "library_search_test_support.hpp"
#include "process_memory_test_support.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;
using process_memory_test::Mebibytes;
using process_memory_test::PrivateBytes;

constexpr size_t kBatchSize  = 500;
constexpr size_t kBatchCount = 8;

/// `count` RAW files with distinct names; every 10th one is a DNG with the large profile.
auto LibrarySpecs(size_t count) -> std::vector<SyntheticImageSpec> {
  std::vector<SyntheticImageSpec> specs;
  specs.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    SyntheticImageSpec spec;
    spec.has_dng_profile_ = i % 10 == 0;
    spec.file_name_  = std::format(L"DSC_{:06}.{}", i, spec.has_dng_profile_ ? L"dng" : L"NEF");
    spec.image_path_ = std::format(L"D:/photos/trip_{:03}/", i / 250) + spec.file_name_;
    spec.make_       = "NIKON CORPORATION";
    spec.model_      = "NIKON Z 8";
    spec.lens_       = "NIKKOR Z 24-70mm f/4 S";
    spec.date_time_  = std::format("2025-01-{:02} 10:{:02}:00", 1 + i % 28, i % 60);
    spec.iso_        = 100 * (1 + i % 8);
    spec.aperture_   = 4.0f;
    spec.focal_      = 50.0f;
    spec.rating_     = static_cast<int>(i % 6);
    specs.push_back(std::move(spec));
  }
  return specs;
}

/// Add @p specs to the library root in 500-file batches (one image pool sync and one Sleeve
/// write each) and return the private bytes that each batch added.
auto BuildInBatches(ProjectService& project, const std::vector<SyntheticImageSpec>& specs)
    -> std::vector<int64_t> {
  SyntheticLibraryBuilder builder(project);
  std::vector<int64_t>    added_bytes;
  for (size_t begin = 0; begin < specs.size(); begin += kBatchSize) {
    const std::vector<SyntheticImageSpec> batch(
        specs.begin() + static_cast<std::ptrdiff_t>(begin),
        specs.begin() + static_cast<std::ptrdiff_t>(std::min(specs.size(), begin + kBatchSize)));
    const auto before = PrivateBytes();
    EXPECT_EQ(builder.AddFiles(batch).size(), batch.size());
    added_bytes.push_back(PrivateBytes() - before);
  }
  return added_bytes;
}

class LibraryBuildMemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    work_dir_ = std::filesystem::temp_directory_path() / "library_build_memory_test";
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
    std::filesystem::create_directories(work_dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
  }

  std::filesystem::path work_dir_;
};

// Each batch writes the same number of rows, so it adds about the same memory. Before this
// phase every DuckDB DML result leaked and the library folder rewrote all its content rows on
// each sync: batch 1 added 85 MB, batch 8 190 MB.
TEST_F(LibraryBuildMemoryTest, LibraryBuildMemoryGrowsLinearly) {
  ProjectService project(work_dir_ / "linear.db", work_dir_ / "linear.json");
  const auto     added_bytes = BuildInBatches(project, LibrarySpecs(kBatchSize * kBatchCount));
  ASSERT_EQ(added_bytes.size(), kBatchCount);

  std::string report = "private MiB added per 500-file batch:";
  for (const auto bytes : added_bytes) {
    report += std::format(" {:.1f}", Mebibytes(bytes));
  }
  std::cout << report << "\n";
  RecordProperty("added_mib_per_batch", report);

  // The first batch fills an empty image pool and grows the tables from zero; the batch limit
  // is stated against it as in the plan. A floor of 4 MiB keeps allocator noise from failing
  // a batch that adds almost nothing.
  const auto first_batch = std::max<int64_t>(added_bytes.front(), 4 * 1024 * 1024);
  EXPECT_LE(static_cast<double>(added_bytes.back()), 1.5 * static_cast<double>(first_batch))
      << report;
}

// Destroying the ProjectService returns the memory the library build allocated. A first
// project is opened and closed before the measurement, so one-time process state (DuckDB
// extension loading, static caches) is not counted.
TEST_F(LibraryBuildMemoryTest, ProjectCloseReleasesLibraryBuildMemory) {
  constexpr int64_t kToleranceBytes = 32 * 1024 * 1024;
  {
    ProjectService warm_up(work_dir_ / "warm_up.db", work_dir_ / "warm_up.json");
    BuildInBatches(warm_up, LibrarySpecs(kBatchSize));
  }

  const auto before_open = PrivateBytes();
  int64_t    after_build = 0;
  {
    ProjectService project(work_dir_ / "close.db", work_dir_ / "close.json");
    BuildInBatches(project, LibrarySpecs(kBatchSize * kBatchCount));
    after_build = PrivateBytes();
  }
  const auto after_close = PrivateBytes();

  const auto report = std::format("private MiB: before open {:.1f}, after build {:.1f}, after "
                                  "close {:.1f}",
                                  Mebibytes(before_open), Mebibytes(after_build),
                                  Mebibytes(after_close));
  std::cout << report << "\n";
  RecordProperty("private_mib", report);
  EXPECT_LE(after_close - before_open, kToleranceBytes) << report;
}

// Diagnostic for Phase S6 step 1: the private bytes that each step of a Sleeve sync adds, for
// eight 500-file batches. Runs the steps of SleeveServiceImpl::Sync one by one inside the
// write, so the sync that follows has nothing left to write.
TEST_F(LibraryBuildMemoryTest, DISABLED_ReportsPrivateBytesOfEachSleeveSyncStep) {
  ProjectService project(work_dir_ / "steps.db", work_dir_ / "steps.json");
  const auto     specs         = LibrarySpecs(kBatchSize * kBatchCount);
  auto           image_pool    = project.GetImagePoolService();
  auto&          element_store = project.GetStorage()->GetElementStore();
  std::cout << std::format("{:>5} {:>9} {:>9} {:>9} {:>9} {:>9} {:>9}\n", "batch", "pool", "fs",
                           "add", "update", "gc", "total");
  for (size_t batch = 0; batch < kBatchCount; ++batch) {
    const auto begin = batch * kBatchSize;
    const auto start = PrivateBytes();
    std::vector<image_id_t> image_ids;
    {
      std::vector<ImagePoolManager::PinnedImageHandle> pinned;
      for (size_t i = begin; i < begin + kBatchSize; ++i) {
        auto handle               = image_pool->CreateAndReturnPinnedEmpty();
        handle.Get()->image_name_ = specs[i].file_name_;
        handle.Get()->image_path_ = specs[i].image_path_;
        image_ids.push_back(handle.Get()->image_id_);
        pinned.push_back(std::move(handle));
      }
      image_pool->SyncWithStorage();
    }
    const auto after_pool = PrivateBytes();
    int64_t    after_fs = 0, after_add = 0, after_update = 0, after_gc = 0;
    project.GetSleeveService()->Write<void>([&](FileSystem& fs) {
      for (size_t i = begin; i < begin + kBatchSize; ++i) {
        fs.CreateFileInLibrary(specs[i].file_name_)->image_id_ = image_ids[i - begin];
      }
      after_fs = PrivateBytes();
      element_store.AddElements(fs.GetUnsyncedElements());
      after_add = PrivateBytes();
      element_store.UpdateElements(fs.GetModifiedElements());
      after_update = PrivateBytes();
      fs.GarbageCollect();
      after_gc = PrivateBytes();
    });
    std::cout << std::format("{:>5} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f}\n",
                             batch + 1, Mebibytes(after_pool - start),
                             Mebibytes(after_fs - after_pool), Mebibytes(after_add - after_fs),
                             Mebibytes(after_update - after_add),
                             Mebibytes(after_gc - after_update), Mebibytes(PrivateBytes() - start));
  }
}

}  // namespace
}  // namespace alcedo
