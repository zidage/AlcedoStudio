//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Project open latency benchmark over a real packed project.
//
// Input: ALCEDO_PROJECT_OPEN_BENCH_FILE holds the path of one .alcd file. The test is skipped
// when the variable is not set. ALCEDO_PROJECT_OPEN_BENCH_RUNS sets the number of open/close
// rounds (default 3).
//
// Each round runs the steps the application runs while the "Loading Project" indicator shows:
// unpack on the UI thread, the service construction of ProjectHandler::InitializeServices, and
// the first folder tree, stats, and thumbnail page queries of the project_opened hook. It also
// times the release of the services, which the next project open pays for.
//
// Run a Release build: Debug timings say nothing about open latency.

#include <gtest/gtest.h>

#include <QString>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_service.hpp"

namespace alcedo {
namespace {

using BenchClock = std::chrono::steady_clock;

auto ElapsedMs(BenchClock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(BenchClock::now() - start).count();
}

auto ReadBenchProjectFile() -> std::filesystem::path {
#if defined(_WIN32)
  const wchar_t* raw = _wgetenv(L"ALCEDO_PROJECT_OPEN_BENCH_FILE");
  return raw == nullptr ? std::filesystem::path{} : std::filesystem::path(raw);
#else
  const char* raw = std::getenv("ALCEDO_PROJECT_OPEN_BENCH_FILE");
  return raw == nullptr ? std::filesystem::path{} : std::filesystem::path(raw);
#endif
}

auto ReadBenchRunCount() -> int {
  const char* raw = std::getenv("ALCEDO_PROJECT_OPEN_BENCH_RUNS");
  if (raw == nullptr) {
    return 3;
  }
  const int runs = std::atoi(raw);
  return runs > 0 ? runs : 3;
}

class StepTimer {
 public:
  explicit StepTimer(int round) : round_(round) {}

  template <typename Fn>
  void Run(const char* step_name, Fn&& fn) {
    const auto start = BenchClock::now();
    fn();
    const double ms = ElapsedMs(start);
    total_ms_ += ms;
    std::cout << "[project-open] round=" << round_ << " step=" << step_name << " ms=" << ms << "\n";
  }

  [[nodiscard]] auto total_ms() const -> double { return total_ms_; }

 private:
  int    round_;
  double total_ms_ = 0.0;
};

TEST(ProjectOpenBenchmark, OpensPackedProjectAndReportsPerStepLatency) {
  const auto project_file = ReadBenchProjectFile();
  if (project_file.empty()) {
    GTEST_SKIP() << "ALCEDO_PROJECT_OPEN_BENCH_FILE is not set.";
  }
  ASSERT_TRUE(std::filesystem::is_regular_file(project_file)) << project_file.string();

  const int             runs = ReadBenchRunCount();
  ProjectPackageService package_service;
  const QString         project_name = QStringLiteral("project_open_benchmark");

  for (int round = 0; round < runs; ++round) {
    StepTimer                            timer(round);
    std::filesystem::path                workspace_dir;
    std::filesystem::path                db_path;
    std::filesystem::path                meta_path;

    std::shared_ptr<ProjectService>      project;
    std::shared_ptr<PipelineMgmtService> pipeline;
    std::shared_ptr<ThumbnailService>    thumbnail;
    std::unique_ptr<ImportServiceImpl>   import_service;
    std::shared_ptr<ExportService>       export_service;

    timer.Run("unpack", [&] {
      QString error;
      ASSERT_TRUE(package_service.CreateProjectWorkspace(project_name, &workspace_dir, &error))
          << error.toStdString();
      ASSERT_TRUE(package_service.UnpackProjectToWorkspace(
          project_file, workspace_dir, project_name, &db_path, &meta_path, &error))
          << error.toStdString();
    });
    timer.Run("project_service", [&] {
      project =
          std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    });
    timer.Run("pipeline_service",
              [&] { pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage()); });
    timer.Run("thumbnail_service", [&] {
      thumbnail = std::make_shared<ThumbnailService>(
          project->GetSleeveService(), project->GetImagePoolService(), pipeline,
          project->GetStorage(), project->GetProjectUUID());
    });
    timer.Run("import_export_service", [&] {
      import_service = std::make_unique<ImportServiceImpl>(
          project->GetSleeveService(), project->GetImagePoolService(), pipeline);
      export_service = std::make_shared<ExportService>(project->GetSleeveService(),
                                                       project->GetImagePoolService(), pipeline);
    });

    auto browse = project->GetAlbumBrowseService();
    auto filter = project->GetSleeveFilterService();
    ASSERT_NE(browse, nullptr);
    ASSERT_NE(filter, nullptr);
    const sl_element_id_t root_folder_id = 0;

    size_t                folder_count   = 0;
    size_t                file_count     = 0;
    size_t                page_count     = 0;
    timer.Run("folder_tree",
              [&] { folder_count = browse->ListFolders(std::filesystem::path(L"/")).size(); });
    timer.Run("folder_stats", [&] { (void)filter->BuildFolderStats(root_folder_id); });
    timer.Run("file_count", [&] { file_count = browse->CountFilesInFolderById(root_folder_id); });
    timer.Run("first_file_page",
              [&] { page_count = browse->ListFilesInFolderById(root_folder_id, 0, 1000).size(); });

    std::cout << "[project-open] round=" << round << " folders=" << folder_count
              << " files=" << file_count << " first_page=" << page_count
              << " open_total_ms=" << timer.total_ms() << "\n";

    StepTimer close_timer(round);
    close_timer.Run("release_services", [&] {
      export_service.reset();
      import_service.reset();
      thumbnail.reset();
      pipeline.reset();
      browse.reset();
      filter.reset();
      project.reset();
    });

    std::error_code ec;
    std::filesystem::remove_all(workspace_dir, ec);
  }
}

}  // namespace
}  // namespace alcedo
