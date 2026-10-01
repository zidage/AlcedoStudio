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
#include <span>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <psapi.h>
#include <windows.h>
#endif

#include "app/album_browse_service.hpp"
#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"
#include "decoders/dng_default_crop.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"
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


// Project open, step by step, as ProjectModule::LoadProject + ProjectHandler::InitializeServices
// run it, then the first library page query and project close. Opens
// ALCEDO_IMPORT_BENCH_OPEN_PACKAGE when it is set (unpacked into a temp workspace and never
// written back); otherwise imports the first folder of ALCEDO_IMPORT_BENCH_DIRS and packs it.
// The thumbnail service opens a temporary copy of the project's directory in the user's
// thumbnail cache, so it reads an index of the real size and never writes the user's cache.
TEST(ImportThroughputBenchmark, OpensProjectAndReportsStepTimings) {
  TimeProvider::Refresh();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  const auto work_dir = std::filesystem::temp_directory_path() / "alcedo_open_benchmark";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  std::filesystem::path package_path;
#if defined(_WIN32)
  if (const wchar_t* raw = _wgetenv(L"ALCEDO_IMPORT_BENCH_OPEN_PACKAGE")) {
    package_path = raw;
  }
#else
  if (const char* raw = std::getenv("ALCEDO_IMPORT_BENCH_OPEN_PACKAGE")) {
    package_path = raw;
  }
#endif
  if (package_path.empty()) {
    const auto folders = ReadBenchFolders();
    if (folders.empty()) {
      GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS and ALCEDO_IMPORT_BENCH_OPEN_PACKAGE are not set";
    }
    package_path   = work_dir / "imported.alcd";
    const auto db  = work_dir / "imported.db";
    const auto meta = work_dir / "imported.json";
    auto project   = std::make_shared<ProjectService>(db, meta);
    auto pipeline  = std::make_shared<PipelineMgmtService>(project->GetStorage());
    auto importer  = std::make_unique<ImportServiceImpl>(project->GetSleeveService(),
                                                        project->GetImagePoolService(), pipeline);
    auto job       = std::make_shared<ImportJob>();
    std::promise<void> finished;
    job->on_finished_ = [&finished](const ImportResult&) { finished.set_value(); };
    job               = importer->ImportToFolder(CollectFolderFiles(folders.front()), L"", {}, job);
    finished.get_future().wait();
    importer->SyncImports(job->import_log_->Snapshot(), L"");
    project->GetSleeveService()->Sync();
    project->GetImagePoolService()->SyncWithStorage();
    project->SaveProject(meta);
    QString error;
    const auto snapshot = work_dir / "imported_snapshot.db";
    ASSERT_TRUE(project->GetProjectPackageService()->CreateLiveDbSnapshot(project, snapshot, &error))
        << error.toStdString();
    ASSERT_TRUE(
        project->GetProjectPackageService()->WritePackedProject(package_path, meta, snapshot, &error))
        << error.toStdString();
    importer.reset();
    pipeline.reset();
    project.reset();
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(package_path)) << package_path;

  for (int round = 0; round < 2; ++round) {
    ProjectPackageService package_service;
    const auto            open_start = BenchClock::now();
    auto                  step_start = open_start;
    std::filesystem::path workspace;
    QString               error;
    ASSERT_TRUE(package_service.CreateProjectWorkspace(QStringLiteral("open_benchmark"),
                                                       &workspace, &error))
        << error.toStdString();
    std::filesystem::path db_path;
    std::filesystem::path meta_path;
    ASSERT_TRUE(package_service.UnpackProjectToWorkspace(package_path, workspace,
                                                         QStringLiteral("open_benchmark"),
                                                         &db_path, &meta_path, &error))
        << error.toUtf8().constData();
    const auto unpack_ms = ElapsedMs(step_start);

    step_start = BenchClock::now();
    auto project =
        std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    const auto project_ms = ElapsedMs(step_start);

    step_start    = BenchClock::now();
    auto pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage());
    const auto pipeline_ms = ElapsedMs(step_start);

    // Open-path time so far; the cache copy below is test setup and is not counted.
    const auto open_without_copy_ms = ElapsedMs(open_start);
    const auto cache_root = work_dir / ("cache_" + std::to_string(round));
    std::filesystem::create_directories(cache_root);
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
      const auto user_cache = std::filesystem::path(local_app_data) / "alcedo" / "thumbnails" /
                              project->GetProjectUUID();
      if (std::filesystem::is_directory(user_cache)) {
        std::filesystem::copy(user_cache, cache_root / project->GetProjectUUID(),
                              std::filesystem::copy_options::recursive);
      }
    }

    step_start     = BenchClock::now();
    auto thumbnail = std::make_shared<ThumbnailService>(
        project->GetSleeveService(), project->GetImagePoolService(), pipeline,
        project->GetStorage(), project->GetProjectUUID(), cache_root);
    const auto thumbnail_ms = ElapsedMs(step_start);

    step_start    = BenchClock::now();
    auto importer = std::make_unique<ImportServiceImpl>(project->GetSleeveService(),
                                                        project->GetImagePoolService(), pipeline);
    auto exporter = std::make_shared<ExportService>(project->GetSleeveService(),
                                                    project->GetImagePoolService(), pipeline);
    const auto import_export_ms = ElapsedMs(step_start);
    const auto services_ms      = open_without_copy_ms + thumbnail_ms + import_export_ms;

    step_start         = BenchClock::now();
    auto       browse  = project->GetAlbumBrowseService();
    const auto count   = browse->CountFilesInFolderById(0, std::nullopt);
    const auto page    = browse->ListFilesInFolderById(0, 0, 200);
    const auto page_ms = ElapsedMs(step_start);

    step_start = BenchClock::now();
    exporter.reset();
    importer.reset();
    thumbnail.reset();
    pipeline.reset();
    project.reset();
    const auto close_ms = ElapsedMs(step_start);

    std::cout << "\n[open-benchmark] round " << round << " package: " << package_path.string()
              << " (" << std::filesystem::file_size(package_path) << " bytes, " << count
              << " files in root)\n"
              << "  workspace + unpack  : " << unpack_ms << " ms\n"
              << "  ProjectService load : " << project_ms << " ms\n"
              << "  PipelineMgmtService : " << pipeline_ms << " ms\n"
              << "  ThumbnailService    : " << thumbnail_ms << " ms\n"
              << "  Import + Export svc : " << import_export_ms << " ms\n"
              << "  services ready      : " << services_ms << " ms\n"
              << "  root count + page   : " << page_ms << " ms (" << page.size() << " rows)\n"
              << "  close               : " << close_ms << " ms\n"
              << "  peak working set    : " << PeakWorkingSetMiB() << " MiB\n";
    std::error_code ec;
    std::filesystem::remove_all(workspace, ec);
  }
  std::filesystem::remove_all(work_dir);
}


// First library page after project open: request the thumbnails of the first
// ALCEDO_IMPORT_BENCH_THUMBNAILS files (default 60) at k512 and time each until it is ready.
// Opens the project twice on one temporary disk cache: first with the cache empty (cold), then
// again with the thumbnails the first open wrote (second open). The user's cache is not used.
// Needs ALCEDO_IMPORT_BENCH_OPEN_PACKAGE.
TEST(ImportThroughputBenchmark, ReportsFirstPageThumbnailTimings) {
  std::filesystem::path package_path;
#if defined(_WIN32)
  if (const wchar_t* raw = _wgetenv(L"ALCEDO_IMPORT_BENCH_OPEN_PACKAGE")) {
    package_path = raw;
  }
#endif
  if (package_path.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_OPEN_PACKAGE is not set";
  }
  size_t request_count = 60;
  if (const char* raw = std::getenv("ALCEDO_IMPORT_BENCH_THUMBNAILS")) {
    request_count = static_cast<size_t>(std::max(1, std::atoi(raw)));
  }
  TimeProvider::Refresh();
  Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  const auto work_dir = std::filesystem::temp_directory_path() / "alcedo_thumbnail_benchmark";
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);

  const auto cache_root = work_dir / "cache";
  std::filesystem::create_directories(cache_root);
  for (const bool warm : {false, true}) {
    ProjectPackageService package_service;
    std::filesystem::path workspace;
    QString               error;
    ASSERT_TRUE(package_service.CreateProjectWorkspace(QStringLiteral("thumbnail_benchmark"),
                                                       &workspace, &error));
    std::filesystem::path db_path;
    std::filesystem::path meta_path;
    ASSERT_TRUE(package_service.UnpackProjectToWorkspace(package_path, workspace,
                                                         QStringLiteral("thumbnail_benchmark"),
                                                         &db_path, &meta_path, &error))
        << error.toUtf8().constData();
    auto project =
        std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    auto pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage());

    size_t cached_files = 0;
    if (std::filesystem::is_directory(cache_root / project->GetProjectUUID())) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(
               cache_root / project->GetProjectUUID())) {
        cached_files += entry.is_regular_file() ? 1 : 0;
      }
    }

    const auto service_start = BenchClock::now();
    auto       thumbnail     = std::make_shared<ThumbnailService>(
        project->GetSleeveService(), project->GetImagePoolService(), pipeline,
        project->GetStorage(), project->GetProjectUUID(), cache_root);
    const auto service_ms = ElapsedMs(service_start);

    const auto page = project->GetAlbumBrowseService()->ListFilesInFolderById(0, 0, request_count);
    ASSERT_FALSE(page.empty());

    std::mutex              mutex;
    std::condition_variable done_cv;
    size_t                  done = 0, ready = 0;
    std::vector<double>     latencies;
    const auto              request_start = BenchClock::now();
    for (const auto& file : page) {
      thumbnail->GetThumbnailDetailed(
          file.element_id_, file.image_id_,
          [&](ThumbnailRequestResult result) {
            std::lock_guard lock(mutex);
            latencies.push_back(ElapsedMs(request_start));
            ready += result.status == ThumbnailRequestStatus::kReady && result.guard ? 1 : 0;
            if (!(result.status == ThumbnailRequestStatus::kReady && result.guard) && done < 3) {
              std::cout << "  request error: " << result.message << "\n";
            }
            ++done;
            done_cv.notify_all();
          },
          false, nullptr, ThumbnailResolution::k512);
    }
    {
      std::unique_lock lock(mutex);
      done_cv.wait(lock, [&] { return done == page.size(); });
    }
    const auto total_ms = ElapsedMs(request_start);
    std::sort(latencies.begin(), latencies.end());
    std::cout << "\n[thumbnail-benchmark] " << (warm ? "second open" : "cold") << " ("
              << cached_files << " files in the project cache), " << page.size()
              << " thumbnails at k512\n"
              << "  ThumbnailService ctor : " << service_ms << " ms\n"
              << "  first ready           : " << latencies.front() << " ms\n"
              << "  median ready          : " << latencies[latencies.size() / 2] << " ms\n"
              << "  all ready             : " << total_ms << " ms (" << ready << " ready)\n"
              << "  per thumbnail         : " << total_ms / static_cast<double>(page.size())
              << " ms\n"
              << "  peak working set      : " << PeakWorkingSetMiB() << " MiB\n";
    thumbnail.reset();
    pipeline.reset();
    project.reset();
    std::error_code ec;
    std::filesystem::remove_all(workspace, ec);
  }
  std::filesystem::remove_all(work_dir);
}


// Thumbnail RAW decode cost alone: read the file and RawInputLoader::LoadEncoded at the
// decode resolution of each thumbnail tier, on 1 thread and on all logical cores.
TEST(ImportThroughputBenchmark, ReportsThumbnailRawDecodeCost) {
  const auto folders = ReadBenchFolders();
  if (folders.empty()) {
    GTEST_SKIP() << "ALCEDO_IMPORT_BENCH_DIRS is not set";
  }
  for (const auto& folder : folders) {
    const auto paths = CollectRawFiles(folder, 40);
    ASSERT_FALSE(paths.empty());
    for (const auto decode_res : {DecodeRes::EIGHTH, DecodeRes::QUARTER}) {
      for (const unsigned threads : {1u, std::max(2u, std::thread::hardware_concurrency())}) {
        std::atomic<size_t>      next{0};
        std::atomic<long long>   read_us{0}, decode_us{0};
        std::vector<std::thread> workers;
        const auto               start = BenchClock::now();
        for (unsigned t = 0; t < threads; ++t) {
          workers.emplace_back([&]() {
            for (size_t index = next.fetch_add(1); index < paths.size();
                 index         = next.fetch_add(1)) {
              auto              step = BenchClock::now();
              std::ifstream     in(paths[index], std::ios::binary | std::ios::ate);
              std::vector<char> bytes(static_cast<size_t>(in.tellg()));
              in.seekg(0);
              in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
              read_us += static_cast<long long>(ElapsedMs(step) * 1000.0);
              step = BenchClock::now();
              const auto prepared = RawInputLoader::LoadEncoded(
                  std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),
                                             bytes.size()),
                  decode_res);
              (void)prepared;
              decode_us += static_cast<long long>(ElapsedMs(step) * 1000.0);
            }
          });
        }
        for (auto& worker : workers) {
          worker.join();
        }
        const double n = static_cast<double>(paths.size());
        std::cout << "[thumbnail-decode] " << folder.filename().string() << " "
                  << (decode_res == DecodeRes::EIGHTH ? "EIGHTH" : "QUARTER") << ", " << threads
                  << " threads: wall " << ElapsedMs(start) / n << " ms/file, read "
                  << static_cast<double>(read_us) / 1000.0 / n << " ms/file, LoadEncoded "
                  << static_cast<double>(decode_us) / 1000.0 / n << " ms/file\n";
      }
    }
  }
}

}  // namespace alcedo
