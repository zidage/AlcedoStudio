//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Import throughput benchmark and equivalence checks over real photo folders.
//
// Input: ALCEDO_IMPORT_BENCH_DIRS holds one or more folders separated by ';'. Every regular file
// below each folder is used, as the folder import dialog does. Each test is skipped when the
// variable is not set. ALCEDO_IMPORT_BENCH_SAMPLE limits the per-step test (default 40 files).
//
// Run a Release build: Debug timings say nothing about import speed.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <psapi.h>
#include <windows.h>
#endif

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "decoders/dng_default_crop.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using BenchClock = std::chrono::steady_clock;

auto ElapsedMs(BenchClock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(BenchClock::now() - start).count();
}

auto PeakWorkingSetMiB() -> double {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
#endif
  return 0.0;
}

auto ReadBenchFolders() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> folders;
#if defined(_WIN32)
  const wchar_t* raw = _wgetenv(L"ALCEDO_IMPORT_BENCH_DIRS");
  if (raw == nullptr) {
    return folders;
  }
  const std::wstring value(raw);
  const wchar_t      separator = L';';
#else
  const char* raw = std::getenv("ALCEDO_IMPORT_BENCH_DIRS");
  if (raw == nullptr) {
    return folders;
  }
  const std::string value(raw);
  const char        separator = ';';
#endif
  size_t begin = 0;
  while (begin <= value.size()) {
    const size_t end   = value.find(separator, begin);
    const auto   token = value.substr(begin, end == value.npos ? end : end - begin);
    if (!token.empty()) {
      folders.emplace_back(token);
    }
    if (end == value.npos) {
      break;
    }
    begin = end + 1;
  }
  return folders;
}

auto CollectFolderFiles(const std::filesystem::path& folder) -> std::vector<image_path_t> {
  std::vector<image_path_t> files;
  std::error_code           ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    std::error_code file_ec;
    if (it->is_regular_file(file_ec) && !file_ec) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

auto OpenRaw(LibRaw& processor, const image_path_t& path) -> int {
#if defined(_WIN32)
  return processor.open_file(path.wstring().c_str());
#else
  return processor.open_file(path.string().c_str());
#endif
}

auto CollectRawFiles(const std::filesystem::path& folder, size_t limit)
    -> std::vector<image_path_t> {
  std::vector<image_path_t> raw_paths;
  for (const auto& path : CollectFolderFiles(folder)) {
    auto processor = std::make_unique<LibRaw>();
    if (OpenRaw(*processor, path) == LIBRAW_SUCCESS) {
      raw_paths.push_back(path);
    }
    if (raw_paths.size() >= limit) {
      break;
    }
  }
  return raw_paths;
}

}  // namespace

// The same sequence as the album import path (ImportExportHandler::StartImport + FinishImport):
// ImportToFolder, SyncImports, sleeve and image-pool sync, SaveProject and the project package
// write. Reports the wall time of each step and the peak working set.
TEST(ImportThroughputBenchmark, ImportsFoldersAndReportsStepTimings) {
  const auto folders = ReadBenchFolders();
  if (folders.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS is not set";
  }
  TimeProvider::Refresh();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);

  const auto work_dir = std::filesystem::temp_directory_path() / "alcedo_import_benchmark";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);
  const auto db_path       = work_dir / "bench.db";
  const auto meta_path     = work_dir / "bench.json";
  const auto package_path  = work_dir / "bench.alcd";
  const auto snapshot_path = work_dir / "bench_snapshot.db";

  for (const auto& folder : folders) {
    const auto paths = CollectFolderFiles(folder);
    ASSERT_FALSE(paths.empty()) << folder;

    std::filesystem::remove(db_path);
    std::filesystem::remove(meta_path);
    std::filesystem::remove(package_path);

    const auto open_start = BenchClock::now();
    auto       project    = std::make_shared<ProjectService>(db_path, meta_path);
    auto       pipeline   = std::make_shared<PipelineMgmtService>(project->GetStorage());
    auto       importer   = std::make_unique<ImportServiceImpl>(project->GetSleeveService(),
                                                                project->GetImagePoolService(), pipeline);
    const auto open_ms    = ElapsedMs(open_start);

    auto       job        = std::make_shared<ImportJob>();
    std::promise<ImportResult> finished;
    auto                       finished_future = finished.get_future();
    job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };

    const auto import_start = BenchClock::now();
    job                     = importer->ImportToFolder(paths, L"", {}, job);
    const auto submit_ms    = ElapsedMs(import_start);
    const auto result       = finished_future.get();
    const auto import_ms    = ElapsedMs(import_start);

    auto       step_start   = BenchClock::now();
    importer->SyncImports(job->import_log_->Snapshot(), L"");
    const auto sync_imports_ms = ElapsedMs(step_start);

    step_start                 = BenchClock::now();
    project->GetSleeveService()->Sync();
    project->GetImagePoolService()->SyncWithStorage();
    project->SaveProject(meta_path);
    const auto save_ms = ElapsedMs(step_start);

    step_start         = BenchClock::now();
    auto    package    = project->GetProjectPackageService();
    QString error;
    std::filesystem::remove(snapshot_path);
    ASSERT_TRUE(package->CreateLiveDbSnapshot(project, snapshot_path, &error))
        << error.toStdString();
    ASSERT_TRUE(package->WritePackedProject(package_path, meta_path, snapshot_path, &error))
        << error.toStdString();
    std::filesystem::remove(snapshot_path);
    const auto package_ms = ElapsedMs(step_start);
    const auto total_ms   = ElapsedMs(import_start);

    std::cout << "\n[import-benchmark] folder: " << folder.string() << "\n"
              << "  files requested     : " << result.requested_ << "\n"
              << "  imported / failed   : " << result.imported_ << " / " << result.failed_ << "\n"
              << "  project open        : " << open_ms << " ms\n"
              << "  submit (caller)     : " << submit_ms << " ms\n"
              << "  ImportToFolder done : " << import_ms << " ms\n"
              << "  SyncImports         : " << sync_imports_ms << " ms\n"
              << "  sync + SaveProject  : " << save_ms << " ms\n"
              << "  package snapshot    : " << package_ms << " ms\n"
              << "  TOTAL (import..pack): " << total_ms << " ms\n"
              << "  db size             : " << std::filesystem::file_size(db_path) << " bytes\n"
              << "  peak working set    : " << PeakWorkingSetMiB() << " MiB\n";

    EXPECT_EQ(result.requested_, paths.size());
    EXPECT_GT(result.imported_, 0u);

    importer.reset();
    pipeline.reset();
    project.reset();
  }
  std::filesystem::remove_all(work_dir);
}

// Sequential cost per RAW file of each import step, one thread, so the numbers are per-file
// CPU + I/O cost without contention. LibRaw unpack is reported as the cost import no longer pays.
TEST(ImportThroughputBenchmark, ReportsSequentialPerStepCost) {
  const auto folders = ReadBenchFolders();
  if (folders.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS is not set";
  }
  TimeProvider::Refresh();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  size_t sample = 40;
  if (const char* raw = std::getenv("ALCEDO_IMPORT_BENCH_SAMPLE")) {
    sample = static_cast<size_t>(std::max(1, std::atoi(raw)));
  }

  const auto work_dir = std::filesystem::temp_directory_path() / "alcedo_import_step_benchmark";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  for (const auto& folder : folders) {
    const auto raw_paths = CollectRawFiles(folder, sample);
    ASSERT_FALSE(raw_paths.empty()) << folder;

    std::filesystem::remove(work_dir / "steps.db");
    std::filesystem::remove(work_dir / "steps.json");
    auto project = std::make_shared<ProjectService>(work_dir / "steps.db", work_dir / "steps.json");
    auto pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage());

    double                        open_ms = 0, unpack_ms = 0, extract_ms = 0, encode_ms = 0;
    std::vector<EncodedImageRoot> roots;
    for (const auto& path : raw_paths) {
      auto processor = std::make_unique<LibRaw>();
      auto start     = BenchClock::now();
      ASSERT_EQ(OpenRaw(*processor, path), LIBRAW_SUCCESS) << path;
      open_ms += ElapsedMs(start);
      start = BenchClock::now();
      (void)processor->unpack();
      unpack_ms += ElapsedMs(start);
      processor.reset();

      Image image;
      start = BenchClock::now();
      MetadataExtractor::ExtractEXIF_ToImage(path, image);
      extract_ms += ElapsedMs(start);

      const auto file = project->GetSleeveService()->Write_NoSync<std::shared_ptr<SleeveElement>>(
          [&path](FileSystem& fs) { return fs.CreateFileInLibrary(path.filename().wstring()); });
      start = BenchClock::now();
      roots.push_back(pipeline->EncodeImageRoot(file->element_id_, CreateDefaultPipelineDocument(),
                                                &image.GetRawColorContext()));
      encode_ms += ElapsedMs(start);
    }
    const auto write_start = BenchClock::now();
    pipeline->WriteImageRoots(std::move(roots));
    const double write_ms = ElapsedMs(write_start);

    const double n        = static_cast<double>(raw_paths.size());
    std::cout << "\n[import-step-benchmark] folder: " << folder.string() << " (" << n
              << " RAW files)\n"
              << "  LibRaw open_file            : " << open_ms / n << " ms/file\n"
              << "  LibRaw unpack (not imported): " << unpack_ms / n << " ms/file\n"
              << "  ExtractEXIF_ToImage         : " << extract_ms / n << " ms/file\n"
              << "  EncodeImageRoot             : " << encode_ms / n << " ms/file\n"
              << "  WriteImageRoots (1 batch)   : " << write_ms / n << " ms/file\n";
    pipeline.reset();
    project.reset();
  }
  std::filesystem::remove_all(work_dir);
}

// Import reads color metadata from LibRaw after open_file only. For every RAW file of each
// folder, the open-only cam_mul, pre_mul, cam_xyz, rgb_cam and output sizes must equal the
// values unpack() stores in imgdata.rawdata after it decodes the sensor data, and unpack must
// succeed for every file open_file accepts.
TEST(ImportThroughputBenchmark, OpenFileColorDataMatchesUnpackedColorData) {
  const auto folders = ReadBenchFolders();
  if (folders.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS is not set";
  }
  size_t compared = 0;
  for (const auto& folder : folders) {
    for (const auto& path : CollectRawFiles(folder, SIZE_MAX)) {
      auto opened   = std::make_unique<LibRaw>();
      auto unpacked = std::make_unique<LibRaw>();
      ASSERT_EQ(OpenRaw(*opened, path), LIBRAW_SUCCESS) << path;
      ASSERT_EQ(OpenRaw(*unpacked, path), LIBRAW_SUCCESS) << path;
      if (unpacked->unpack() != LIBRAW_SUCCESS) {
        ADD_FAILURE() << "unpack failed after a successful open: " << path;
        continue;
      }
      const auto& open_color   = opened->imgdata.color;
      const auto& unpack_color = unpacked->imgdata.rawdata.color;
      for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(open_color.cam_mul[i], unpack_color.cam_mul[i]) << path << " cam_mul " << i;
        EXPECT_EQ(open_color.pre_mul[i], unpack_color.pre_mul[i]) << path << " pre_mul " << i;
      }
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
          EXPECT_EQ(open_color.cam_xyz[r][c], unpack_color.cam_xyz[r][c]) << path << " cam_xyz";
        }
      }
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          EXPECT_EQ(open_color.rgb_cam[r][c], unpack_color.rgb_cam[r][c]) << path << " rgb_cam";
        }
      }
      EXPECT_EQ(opened->imgdata.sizes.width, unpacked->imgdata.sizes.width) << path;
      EXPECT_EQ(opened->imgdata.sizes.height, unpacked->imgdata.sizes.height) << path;
      EXPECT_EQ(opened->imgdata.sizes.flip, unpacked->imgdata.sizes.flip) << path;
      ++compared;
    }
  }
  std::cout << "[open-vs-unpack] compared " << compared << " RAW files\n";
  EXPECT_GT(compared, 0u);
}

// Import parses DNG geometry tags from a memory-mapped file. For every DNG of each folder, the
// extracted WarpRectilinear presence must equal dng::ExtractMetadata over a full copy of the
// file, which import read before.
TEST(ImportThroughputBenchmark, MappedDngGeometryMatchesWholeFileRead) {
  const auto folders = ReadBenchFolders();
  if (folders.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS is not set";
  }
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  size_t compared = 0;
  size_t warp     = 0;
  for (const auto& folder : folders) {
    for (const auto& path : CollectRawFiles(folder, SIZE_MAX)) {
      std::wstring extension = path.extension().wstring();
      std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
      if (extension != L".dng") {
        continue;
      }
      Image image;
      MetadataExtractor::ExtractEXIF_ToImage(path, image);

      std::ifstream        whole(path, std::ios::binary | std::ios::ate);
      const auto           size = whole.tellg();
      std::vector<uint8_t> buffer(static_cast<size_t>(size));
      whole.seekg(0, std::ios::beg);
      whole.read(reinterpret_cast<char*>(buffer.data()), size);
      const auto metadata = dng::ExtractMetadata(buffer);

      EXPECT_EQ(image.GetRawColorContext().dng_warp_rectilinear_present_,
                metadata.warp_rectilinear.has_value())
          << path;
      warp += metadata.warp_rectilinear.has_value() ? 1 : 0;
      ++compared;
    }
  }
  std::cout << "[dng-geometry] compared " << compared << " DNG files, " << warp
            << " with WarpRectilinear\n";
  if (compared == 0) {
    GTEST_SKIP() << "no DNG files in ALCEDO_IMPORT_BENCH_DIRS";
  }
}

}  // namespace alcedo
