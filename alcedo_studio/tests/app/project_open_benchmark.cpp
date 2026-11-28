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
//
// Node cache measurement (Sleeve Node Cache Plan, SN6): ALCEDO_SLEEVE_BENCH_MOUNT_DIR holds a
// photo directory and ALCEDO_SLEEVE_BENCH_PROJECT_DIR the directory of the measured project. The
// test is skipped when either variable is not set. When the project directory holds no project,
// the test creates one, mounts the photo directory, and runs the first scan (the import). It then
// opens the project, browses 50 folders, and scans the mount again. After each step it reports
// the elements of the node cache and the memory of the process. During each scan a second thread
// reads the sidebar tree in a loop, as the GUI thread does, and the test reports the time of each
// read. A synthetic fill of a separate NodeCache with the real file names and the real folder
// tree gives the bytes of one file element and of one folder element.

#include <gtest/gtest.h>

#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/mount_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_service.hpp"
#include "sleeve/node_cache.hpp"
#include "sleeve/sleeve_element/sleeve_element_factory.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "sleeve/storage.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/storage_maintenance.hpp"
#include "support/storage_test_access.hpp"

// The platform headers follow the project headers, which include winsock2.h on Windows.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h needs the types of windows.h.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>
#include <unistd.h>

#include <fstream>
#endif

namespace alcedo {
namespace {

using BenchClock = std::chrono::steady_clock;

auto ElapsedMs(BenchClock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(BenchClock::now() - start).count();
}

auto ReadPathVariable(const char* name) -> std::filesystem::path {
#if defined(_WIN32)
  const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
  const wchar_t*     raw = _wgetenv(wide_name.c_str());
  return raw == nullptr ? std::filesystem::path{} : std::filesystem::path(raw);
#else
  const char* raw = std::getenv(name);
  return raw == nullptr ? std::filesystem::path{} : std::filesystem::path(raw);
#endif
}

auto ReadBenchProjectFile() -> std::filesystem::path {
  return ReadPathVariable("ALCEDO_PROJECT_OPEN_BENCH_FILE");
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
    project_pack::ProjectWorkspaceLock   workspace_lock;
    std::filesystem::path                db_path;
    std::filesystem::path                meta_path;

    std::shared_ptr<ProjectService>      project;
    std::shared_ptr<PipelineMgmtService> pipeline;
    std::shared_ptr<ThumbnailService>    thumbnail;
    std::unique_ptr<ImportServiceImpl>   import_service;
    std::shared_ptr<ExportService>       export_service;

    timer.Run("unpack", [&] {
      QString error;
      ASSERT_TRUE(package_service.CreateProjectWorkspace(project_name, &workspace_dir,
                                                         &workspace_lock, &error))
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
    // The store read of the project open (NodeStorageHandler::LoadFolderTree), run once more,
    // and the sidebar read of the resident tree.
    timer.Run("folder_tree_query",
              [&] { (void)project->GetStorage()->GetFolderEntryStore().ReadFolderTree(); });
    timer.Run("folder_tree", [&] { folder_count = browse->ListFolderTree().size(); });
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
    workspace_lock.Release();
    std::filesystem::remove_all(workspace_dir, ec);
  }
}

// ── Node cache measurement (SN6) ───────────────────────────────────────────

constexpr double      kBytesPerMb           = 1024.0 * 1024.0;
/// The first page of a folder in the library grid (LibraryModule, kAlbumMetadataPageSize).
constexpr int64_t     kLibraryPageSize      = 1000;
/// Folders that the browse step opens.
constexpr std::size_t kBrowsedFolders       = 50;
/// The wait of one sidebar read that starts SN8 (plan, section 10).
constexpr double      kSidebarWaitLimitMs   = 50.0;
/// The pause of the sidebar probe between two reads.
constexpr auto        kSidebarProbeInterval = std::chrono::milliseconds(20);

/// Memory of the process: the private bytes (Windows: commit charge; macOS: physical
/// footprint; Linux: resident set) and the resident set.
/// The heap bytes count each live malloc and new allocation of the process (the CRT heap on
/// Windows, the default zone on macOS, the main arena on Linux). They do not drop when a freed
/// block stays in the heap, so a fill after a free is visible in them and not in the private
/// bytes.
struct ProcessMemory {
  std::uint64_t private_bytes  = 0;
  std::uint64_t resident_bytes = 0;
  std::uint64_t heap_bytes     = 0;
};

auto ReadHeapBytes() -> std::uint64_t {
#if defined(_WIN32)
  // The UCRT allocates from the process heap.
  HEAP_SUMMARY summary{};
  summary.cb = sizeof(summary);
  return HeapSummary(GetProcessHeap(), 0, &summary) ? summary.cbAllocated : 0;
#elif defined(__APPLE__)
  malloc_statistics_t statistics{};
  malloc_zone_statistics(nullptr, &statistics);
  return statistics.size_in_use;
#else
  return mallinfo2().uordblks;
#endif
}

auto ReadProcessMemory() -> ProcessMemory {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters)) == 0) {
    return {};
  }
  return {counters.PrivateUsage, counters.WorkingSetSize, ReadHeapBytes()};
#elif defined(__APPLE__)
  task_vm_info_data_t    info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return {};
  }
  return {info.phys_footprint, info.resident_size, ReadHeapBytes()};
#else
  std::ifstream statm("/proc/self/statm");
  std::uint64_t pages    = 0;
  std::uint64_t resident = 0;
  statm >> pages >> resident;
  const auto page_bytes = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
  return {resident * page_bytes, resident * page_bytes, ReadHeapBytes()};
#endif
}

/// The bytes that the DuckDB buffer manager of @p storage holds now, all tags.
auto ReadDuckDbMemoryBytes(Storage& storage) -> std::int64_t {
  auto guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto lock  = guard.Lock();
  return duckorm::scalar<std::int64_t>(
             guard.conn_, duckorm::expr::raw("SELECT COALESCE(SUM(memory_usage_bytes), 0)::BIGINT "
                                             "FROM duckdb_memory()"))
      .value_or(0);
}

struct NodeCacheCounts {
  std::size_t elements = 0;
  std::size_t folders  = 0;
  std::size_t dirty    = 0;
  std::size_t pages    = 0;
};

auto ReadNodeCacheCounts(SleeveServiceImpl& sleeve) -> NodeCacheCounts {
  return sleeve.Read<NodeCacheCounts>([](FileSystem& fs) {
    const auto&     nodes = fs.Nodes();
    NodeCacheCounts counts{
        .elements = nodes.Size(), .dirty = nodes.DirtyCount(), .pages = nodes.PageCount()};
    nodes.ForEachFolder([&counts](SleeveFolder&) { ++counts.folders; });
    return counts;
  });
}

void ReportState(const char* step, ProjectService& project) {
  const auto counts = ReadNodeCacheCounts(*project.GetSleeveService());
  const auto memory = ReadProcessMemory();
  const auto duckdb = ReadDuckDbMemoryBytes(*project.GetStorage());
  std::cout << "[sleeve-bench] state=" << step << " nodes=" << counts.elements
            << " files=" << counts.elements - counts.folders << " folders=" << counts.folders
            << " dirty=" << counts.dirty << " pages=" << counts.pages
            << " private_mb=" << memory.private_bytes / kBytesPerMb
            << " resident_mb=" << memory.resident_bytes / kBytesPerMb
            << " heap_mb=" << memory.heap_bytes / kBytesPerMb
            << " duckdb_buffer_mb=" << static_cast<double>(duckdb) / kBytesPerMb << "\n";
}

/// Reads the sidebar tree in a loop on its own thread, as the GUI thread does while a scan runs,
/// and keeps the time of each read: the wait on the file system lock plus the read.
class SidebarReadProbe {
 public:
  explicit SidebarReadProbe(std::shared_ptr<AlbumBrowseService> browse)
      : browse_(std::move(browse)), thread_([this] { Run(); }) {}
  SidebarReadProbe(const SidebarReadProbe&)                    = delete;
  auto operator=(const SidebarReadProbe&) -> SidebarReadProbe& = delete;
  ~SidebarReadProbe() { (void)Stop(); }

  /// Ends the loop after the current read and returns the time of each read in milliseconds.
  auto Stop() -> std::vector<double> {
    if (thread_.joinable()) {
      stop_.store(true);
      thread_.join();
    }
    return reads_ms_;
  }

  [[nodiscard]] auto failures() const -> std::size_t { return failures_; }

 private:
  void Run() {
    while (!stop_.load()) {
      const auto start = BenchClock::now();
      try {
        (void)browse_->ListFolderTree();
      } catch (...) {
        ++failures_;
      }
      reads_ms_.push_back(ElapsedMs(start));
      std::this_thread::sleep_for(kSidebarProbeInterval);
    }
  }

  std::shared_ptr<AlbumBrowseService> browse_;
  std::vector<double>                 reads_ms_;
  std::size_t                         failures_ = 0;
  std::atomic<bool>                   stop_{false};
  std::thread                         thread_;
};

auto Percentile(std::vector<double> values, double fraction) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

void ReportSidebarReads(const char* step, const std::vector<double>& reads_ms,
                        std::size_t failures) {
  const auto over_limit = std::count_if(reads_ms.begin(), reads_ms.end(),
                                        [](double ms) { return ms > kSidebarWaitLimitMs; });
  const auto largest = reads_ms.empty() ? 0.0 : *std::max_element(reads_ms.begin(), reads_ms.end());
  std::cout << "[sleeve-bench] sidebar=" << step << " reads=" << reads_ms.size()
            << " failures=" << failures << " median_ms=" << Percentile(reads_ms, 0.5)
            << " p95_ms=" << Percentile(reads_ms, 0.95) << " max_ms=" << largest
            << " over_50ms=" << over_limit << "\n";
}

auto RunMountScan(const char* step, MountService& mounts, MountFileImporter& importer,
                  mount_id_t mount_id, std::shared_ptr<AlbumBrowseService> browse)
    -> MountScanResult {
  MountScanRequest request;
  request.mount_id     = mount_id;
  request.importer     = &importer;
  std::size_t reported = 0;
  request.on_progress  = [&reported](const MountScanProgress& progress) {
    if (progress.new_files_done >= reported + 10000 ||
        (progress.new_files_total > 0 && progress.new_files_done == progress.new_files_total)) {
      reported = progress.new_files_done;
      std::cout << "[sleeve-bench] progress=" << progress.new_files_done << "/"
                << progress.new_files_total << "\n"
                << std::flush;
    }
  };

  SidebarReadProbe probe(std::move(browse));
  const auto       start  = BenchClock::now();
  auto             result = mounts.ScanMount(request);
  const double     ms     = ElapsedMs(start);
  const auto       reads  = probe.Stop();

  std::cout << "[sleeve-bench] scan=" << step << " ms=" << ms
            << " status=" << static_cast<int>(result.status)
            << " imported=" << result.imported_count
            << " import_failed=" << result.import_failed_count
            << " unchanged=" << result.unchanged_count
            << " folders_added=" << result.folders_added_count
            << " fingerprint_reads=" << result.fingerprint_read_count << "\n";
  ReportSidebarReads(step, reads, probe.failures());
  return result;
}

auto IsJpegName(const std::filesystem::path& path) -> bool {
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return extension == L".jpg" || extension == L".jpeg";
}

/// The names of the photo files below @p directory, at most @p limit, in listing order.
auto ListPhotoNames(const std::filesystem::path& directory, std::size_t limit)
    -> std::vector<file_name_t> {
  std::vector<file_name_t> names;
  std::error_code          ec;
  for (std::filesystem::recursive_directory_iterator
           it(directory, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end && names.size() < limit; it.increment(ec)) {
    if (ec) {
      break;
    }
    if (it->is_regular_file(ec) && IsJpegName(it->path())) {
      names.push_back(it->path().filename().wstring());
    }
  }
  return names;
}

/// The file elements of the node cache that hold their Image (SleeveFile::GetImage), and those
/// whose Image no other owner holds, so the node keeps it alive.
struct HeldImages {
  std::size_t holding  = 0;
  std::size_t only_ref = 0;
};

auto CountImagesHeldByNodes(SleeveServiceImpl& sleeve) -> HeldImages {
  return sleeve.Read<HeldImages>([](FileSystem& fs) {
    HeldImages held;
    for (const auto id : fs.ListFileIds(kLibraryRootFolderId)) {
      if (!fs.Nodes().Contains(id)) {
        continue;
      }
      // The cache holds the element, so Get loads nothing.
      const auto element = fs.Get(id);
      if (element->type_ != ElementType::FILE) {
        continue;
      }
      const auto image = std::static_pointer_cast<SleeveFile>(element)->GetImage();
      if (image) {
        ++held.holding;
        // This copy and the node.
        if (image.use_count() == 2) {
          ++held.only_ref;
        }
      }
    }
    return held;
  });
}

/// The heap bytes of one Image as the image store reads it, from up to @p sample Images.
auto MeasureImageBytes(Storage& storage, std::size_t sample) -> double {
  std::vector<std::shared_ptr<Image>> images;
  images.reserve(sample);
  const auto before = ReadProcessMemory().heap_bytes;
  for (image_id_t id = 1; images.size() < sample && id <= sample * 2; ++id) {
    try {
      if (auto image = storage.GetImageStore().GetImageById(id)) {
        images.push_back(std::move(image));
      }
    } catch (const std::exception&) {
      // An id with no row.
    }
  }
  const auto after = ReadProcessMemory().heap_bytes;
  return images.empty() ? 0.0
                        : static_cast<double>(after - before) / static_cast<double>(images.size());
}

/// The heap bytes of one file element and of one folder element (MeasureElementBytes).
struct ElementBytes {
  double file   = 0.0;
  double folder = 0.0;
};

/**
 * @brief Fills a separate NodeCache with a copy of the folder tree and with one file element for
 *        each name, and returns the heap bytes that each kind of element adds.
 *
 * The elements are those of the loaders (SleeveElementFactory, SYNCED, inserted into a NodeCache),
 * so the value holds the element, its control block, its name, and its share of the page table
 * and of the parent map. The heap block headers are not in the value. No database read runs
 * during the fill.
 */
auto MeasureElementBytes(const std::vector<FolderTreeEntry>& tree,
                         const std::vector<file_name_t>&     file_names) -> ElementBytes {
  NodeCache                                                      cache;
  std::map<std::filesystem::path, std::shared_ptr<SleeveFolder>> folder_by_path;
  sl_element_id_t                                                next_id = 0;

  const auto make = [&next_id](ElementType type, const file_name_t& name) {
    auto element = SleeveElementFactory::CreateElement(type, next_id++, name);
    element->SetSyncFlag(SyncFlag::SYNCED);
    return element;
  };

  const auto before_folders = ReadProcessMemory().heap_bytes;
  auto       root = std::static_pointer_cast<SleeveFolder>(make(ElementType::FOLDER, L"root"));
  cache.Insert(root);
  folder_by_path[std::filesystem::path(L"/")] = root;
  std::size_t folders                         = 1;
  for (const auto& entry : tree) {
    if (folder_by_path.contains(entry.path_)) {
      continue;
    }
    auto parent_it = folder_by_path.find(entry.path_.parent_path());
    auto parent    = parent_it == folder_by_path.end() ? root : parent_it->second;
    auto folder    = std::static_pointer_cast<SleeveFolder>(make(ElementType::FOLDER, entry.name_));
    if (parent->Contains(folder->element_name_)) {
      continue;
    }
    parent->AddElementToMap(folder, false);
    cache.Insert(folder);
    folder_by_path[entry.path_] = folder;
    ++folders;
  }
  const auto before_files = ReadProcessMemory().heap_bytes;
  for (const auto& name : file_names) {
    cache.Insert(make(ElementType::FILE, name));
  }
  const auto   after_files = ReadProcessMemory().heap_bytes;

  ElementBytes bytes;
  bytes.folder = static_cast<double>(before_files - before_folders) / static_cast<double>(folders);
  if (!file_names.empty()) {
    bytes.file =
        static_cast<double>(after_files - before_files) / static_cast<double>(file_names.size());
  }
  std::cout << "[sleeve-bench] element_bytes file=" << bytes.file << " folder=" << bytes.folder
            << " files=" << file_names.size() << " folders=" << folders
            << " pages=" << cache.PageCount() << "\n";
  return bytes;
}

struct TableNameRow {
  std::string name;

  using Columns = duckorm::RowColumns<&TableNameRow::name>;
};

struct DatabaseBlocks {
  std::int64_t block_size  = 0;
  std::int64_t used_blocks = 0;
  std::int64_t free_blocks = 0;

  using Columns = duckorm::RowColumns<&DatabaseBlocks::block_size, &DatabaseBlocks::used_blocks,
                                      &DatabaseBlocks::free_blocks>;
};

/// The rows of each table (StorageMaintenance::ReadDataSummary), the persistent blocks of each
/// table with its overflow string blocks, and the used and free blocks of the database file.
void ReportTables(Storage& storage, const std::filesystem::path& db_path) {
  for (const auto& table : storage.Maintenance().ReadDataSummary()) {
    std::cout << "[sleeve-bench] table=" << table.table << " rows=" << table.rows << "\n";
  }
  auto guard        = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto lock         = guard.Lock();
  namespace expr    = duckorm::expr;
  const auto blocks = duckorm::query_one<DatabaseBlocks>(
      guard.conn_, expr::raw("SELECT block_size::BIGINT, used_blocks::BIGINT, "
                             "free_blocks::BIGINT FROM pragma_database_size() "
                             "WHERE database_name = current_database()"));
  const auto block_mb = blocks ? static_cast<double>(blocks->block_size) / kBytesPerMb : 0.0;
  const auto tables   = duckorm::query<TableNameRow>(
      guard.conn_, expr::raw("SELECT table_name FROM duckdb_tables() "
                                 "WHERE database_name = current_database() AND NOT temporary"));
  for (const auto& row : tables) {
    const auto& table  = row.name;
    const auto  quoted = expr::escape_string(table);
    const auto  count  = duckorm::scalar<std::int64_t>(
        guard.conn_,
        expr::raw(std::format("SELECT COUNT(DISTINCT block)::BIGINT FROM ("
                                  "SELECT block_id AS block FROM pragma_storage_info('{0}') "
                                  "WHERE persistent AND block_id >= 0 UNION ALL "
                                  "SELECT UNNEST(additional_block_ids) FROM pragma_storage_info('{0}') "
                                  "WHERE persistent)",
                                quoted)));
    if (count.value_or(0) > 0) {
      std::cout << "[sleeve-bench] table_storage=" << table << " blocks=" << *count
                << " mb=" << static_cast<double>(*count) * block_mb << "\n";
    }
  }
  std::error_code ec;
  const auto      db_bytes = std::filesystem::file_size(db_path, ec);
  std::cout << "[sleeve-bench] database_mb=" << (ec ? 0.0 : db_bytes / kBytesPerMb)
            << " used_mb=" << (blocks ? static_cast<double>(blocks->used_blocks) * block_mb : 0.0)
            << " free_mb=" << (blocks ? static_cast<double>(blocks->free_blocks) * block_mb : 0.0)
            << "\n";
}

TEST(ProjectOpenBenchmark, MountedDirectoryReportsNodeCacheMemoryAndSidebarReadWait) {
  const auto mount_dir   = ReadPathVariable("ALCEDO_SLEEVE_BENCH_MOUNT_DIR");
  const auto project_dir = ReadPathVariable("ALCEDO_SLEEVE_BENCH_PROJECT_DIR");
  if (mount_dir.empty() || project_dir.empty()) {
    GTEST_SKIP() << "ALCEDO_SLEEVE_BENCH_MOUNT_DIR or ALCEDO_SLEEVE_BENCH_PROJECT_DIR is not set.";
  }
  ASSERT_TRUE(std::filesystem::is_directory(mount_dir)) << mount_dir.string();
  std::filesystem::create_directories(project_dir);
  const auto db_path   = project_dir / "sleeve_bench.db";
  const auto meta_path = project_dir / "sleeve_bench.json";

  std::cout << "[sleeve-bench] state=start private_mb="
            << ReadProcessMemory().private_bytes / kBytesPerMb << "\n";

  // Step 1: a new project, the mount, and the first scan, which imports each photo.
  if (!std::filesystem::exists(db_path)) {
    auto project =
        std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
    auto       pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage());
    auto       importer = std::make_unique<ImportServiceImpl>(project->GetSleeveService(),
                                                              project->GetImagePoolService(), pipeline);
    const auto mount_id = project->GetMountService()->CreateMount(mount_dir);
    const auto result   = RunMountScan("first", *project->GetMountService(), *importer, mount_id,
                                       project->GetAlbumBrowseService());
    ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
    ASSERT_GT(result.imported_count, 0U);
    ReportState("after_first_scan", *project);
    const auto held = CountImagesHeldByNodes(*project->GetSleeveService());
    std::cout << "[sleeve-bench] images_held_by_nodes=" << held.holding
              << " only_by_nodes=" << held.only_ref << "\n";
    project->SaveProject(meta_path);

    // The heap that each owner frees when it ends, in the order of the application close.
    const auto start        = BenchClock::now();
    const auto release_step = [](const char* owner) {
      std::cout << "[sleeve-bench] released=" << owner
                << " heap_mb=" << ReadProcessMemory().heap_bytes / kBytesPerMb << "\n";
    };
    importer.reset();
    release_step("import_service");
    pipeline.reset();
    release_step("pipeline_service");
    project.reset();
    release_step("project_service");
    std::cout << "[sleeve-bench] release_ms=" << ElapsedMs(start) << "\n";
  }
  const auto closed = ReadProcessMemory();
  std::cout << "[sleeve-bench] state=closed private_mb=" << closed.private_bytes / kBytesPerMb
            << " heap_mb=" << closed.heap_bytes / kBytesPerMb << "\n";

  // Step 2: the project open.
  std::shared_ptr<ProjectService> project;
  auto                            start = BenchClock::now();
  project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
  std::cout << "[sleeve-bench] open_ms=" << ElapsedMs(start) << "\n";
  auto              pipeline = std::make_shared<PipelineMgmtService>(project->GetStorage());
  ImportServiceImpl importer(project->GetSleeveService(), project->GetImagePoolService(), pipeline);
  auto              sleeve = project->GetSleeveService();
  auto              browse = project->GetAlbumBrowseService();
  ASSERT_NE(sleeve, nullptr);
  ASSERT_NE(browse, nullptr);

  start = BenchClock::now();
  (void)project->GetStorage()->GetFolderEntryStore().ReadFolderTree();
  std::cout << "[sleeve-bench] tree_store_read_ms=" << ElapsedMs(start) << "\n";
  start                 = BenchClock::now();
  const auto tree_views = browse->ListFolderTree();
  std::cout << "[sleeve-bench] sidebar_tree_read_ms=" << ElapsedMs(start)
            << " folders=" << tree_views.size() << "\n";
  start             = BenchClock::now();
  const auto counts = browse->ReadFolderImageCounts();
  std::cout << "[sleeve-bench] folder_counts_read_ms=" << ElapsedMs(start)
            << " photos=" << counts.project_photo_count_ << "\n";
  ReportState("after_open", *project);

  // Step 3: the first library page of 50 folders that hold photos, in sidebar order.
  std::size_t browsed = 0;
  double      slowest = 0.0;
  start               = BenchClock::now();
  for (const auto& view : tree_views) {
    if (browsed == kBrowsedFolders) {
      break;
    }
    if (view.folder_id_ == kLibraryRootFolderId ||
        !counts.folder_photo_counts_.contains(view.folder_id_)) {
      continue;
    }
    const auto folder_start = BenchClock::now();
    (void)browse->ReadAlbumQuery(AlbumScope(view.folder_id_), std::nullopt, AlbumQueryOptions{},
                                 std::string{},
                                 AlbumQueryRead{.offset_              = 0,
                                                .limit_               = kLibraryPageSize,
                                                .read_groups_         = true,
                                                .read_statistics_     = true,
                                                .read_match_file_ids_ = true});
    slowest = std::max(slowest, ElapsedMs(folder_start));
    ++browsed;
  }
  std::cout << "[sleeve-bench] browse folders=" << browsed << " ms=" << ElapsedMs(start)
            << " slowest_ms=" << slowest << "\n";
  ReportState("after_browse", *project);

  // Step 4: a scan of the mount with no change on the disk.
  const auto mounts = project->GetMountService();
  const auto list   = mounts->ListMounts();
  ASSERT_EQ(list.size(), 1U);
  const auto result = RunMountScan("second", *mounts, importer, list.front().record.id, browse);
  ASSERT_EQ(result.status, MountScanStatus::kComplete) << result.error;
  ReportState("after_second_scan", *project);

  // Step 5: the stored tables and the bytes of one element.
  ReportTables(*project->GetStorage(), db_path);
  const auto file_count = static_cast<std::size_t>(counts.project_photo_count_);
  const auto tree       = sleeve->ListFolderTree();
  const auto names      = ListPhotoNames(mount_dir, file_count);
  const auto bytes      = MeasureElementBytes(tree, names);
  std::cout << "[sleeve-bench] image_bytes=" << MeasureImageBytes(*project->GetStorage(), 2000)
            << "\n";
  const auto nodes = ReadNodeCacheCounts(*sleeve);
  std::cout << "[sleeve-bench] node_mb after_second_scan="
            << (bytes.folder * static_cast<double>(nodes.folders) +
                bytes.file * static_cast<double>(nodes.elements - nodes.folders)) /
                   kBytesPerMb
            << " all_files_resident="
            << (bytes.folder * static_cast<double>(nodes.folders) +
                bytes.file * static_cast<double>(file_count)) /
                   kBytesPerMb
            << "\n";
}

}  // namespace
}  // namespace alcedo
