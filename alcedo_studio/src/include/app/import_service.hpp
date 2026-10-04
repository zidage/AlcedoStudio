//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "app/sleeve_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "image_pool_service.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"
#include "utils/import/import_error_code.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo {
class Image;
class PipelineMgmtService;

enum class ImportSortMode : uint8_t {
  NONE      = 0,
  FILE_NAME = 1,  // DEFAULT
  FULL_PATH = 2
};

struct ImportOptions {
  ImportSortMode sort_mode_                        = ImportSortMode::FILE_NAME;

  bool           persist_placeholders_immediately_ = false;

  // If set, record the sequence for deterministic import order.
  bool           write_import_sequence_            = true;
};

struct ImportProgress {
  uint32_t              total_                = 0;

  // Phase A (fast): created SleeveFile nodes + bindings (image_id -> SleeveFile)
  std::atomic<uint32_t> placeholders_created_ = 0;

  // Phase B (slow): metadata extracted + ImagePool updated + DB updated (as applicable)
  std::atomic<uint32_t> metadata_done_        = 0;

  std::atomic<uint32_t> failed_               = 0;

  /// Subset of failed_: files whose content is not a supported RAW file. These are expected in
  /// a folder import (sidecars, JPEG copies, videos) and are reported as skipped, not as errors.
  std::atomic<uint32_t> unsupported_          = 0;
};

struct ImportError {
  ImportErrorCode code_ = ImportErrorCode::UNKNOWN;
  image_path_t    path_{};
  std::string     message_{};
};

struct ImportResult {
  uint32_t requested_ = 0;
  uint32_t imported_  = 0;
  uint32_t failed_    = 0;
  /// Subset of failed_: files that are not a supported RAW file.
  uint32_t unsupported_ = 0;
};

class ImportJob {
 public:
  using ProgressCallback = std::function<void(const ImportProgress&)>;
  using FinishedCallback = std::function<void(const ImportResult&)>;

  // Cancellation toke observed by implementation
  std::atomic<bool>          canceled_{false};
  std::atomic<bool>          cancelation_acked_{false};
  std::atomic<bool>          submission_closed_{false};
  std::atomic<uint32_t>      metadata_tasks_submitted_{0};
  std::atomic<uint32_t>      metadata_tasks_finished_{0};

  ProgressCallback           on_progress_{};
  FinishedCallback           on_finished_{};

  std::shared_ptr<ImportLog> import_log_ = nullptr;

  /// Live counters of this job, set by ImportToFolder. A caller that coalesces on_progress_
  /// reports reads the latest counts here instead of keeping its own copy.
  std::shared_ptr<ImportProgress> progress_ = nullptr;

  ~ImportJob()                           = default;

  auto IsCancelled() const -> bool { return canceled_.load(); }
  auto IsCancelationAcked() const -> bool { return cancelation_acked_.load(); }
};

class ImportService {
 public:
  virtual ~ImportService() = default;

  virtual auto ImportToFolder(const std::vector<image_path_t>& paths, const image_path_t& dest,
                              const ImportOptions&       options = {},
                              std::shared_ptr<ImportJob> job     = nullptr)
      -> std::shared_ptr<ImportJob>                                                         = 0;

  /// Persist a finished import. Each `metadata_failed_` entry loses its library element and
  /// its placeholder Image, so a failed import writes no Element, FileImage, or Image row;
  /// then the image pool and the sleeve file system are written to storage.
  /// Throws when the image store rejects the removal of failed Images.
  virtual void SyncImports(const ImportLogSnapshot& log_snapshot, const image_path_t& dest) = 0;
};

class ImportServiceImpl final : public ImportService {
 public:
  ImportServiceImpl() = delete;
  /// @throws std::invalid_argument when @p pipeline_service is null: every imported image gets
  ///         its history root at import, and only the pipeline service can create it.
  ImportServiceImpl(std::shared_ptr<SleeveServiceImpl>   fs_service,
                    std::shared_ptr<ImagePoolService>    image_pool_service,
                    std::shared_ptr<PipelineMgmtService> pipeline_service)
      : fs_service_(std::move(fs_service)),
        image_pool_service_(std::move(image_pool_service)),
        pipeline_service_(std::move(pipeline_service)) {
    if (!pipeline_service_) {
      throw std::invalid_argument("ImportServiceImpl: pipeline service is required");
    }
  }

  ~ImportServiceImpl() = default;

  std::shared_ptr<SleeveServiceImpl>    fs_service_;

  std::shared_ptr<ImagePoolService> image_pool_service_ = nullptr;

  /// Assembles and saves full operator params (including image-local RAW/lens/CCT) and creates
  /// the image's history root after metadata extraction succeeds.
  std::shared_ptr<PipelineMgmtService> pipeline_service_ = nullptr;

  /// Metadata workers: open RAW headers, read the HDR probe bytes and encode history roots.
  /// The work is short file reads plus CPU, so it scales with the logical core count.
  ThreadPool thread_pool_{std::max<size_t>(8, std::thread::hardware_concurrency())};

  /// Runs the placeholder loop of each import (sleeve element + pinned Image per file, then one
  /// metadata task on thread_pool_), so ImportToFolder returns without walking the file list on
  /// the caller's (UI) thread. Declared after thread_pool_: it is destroyed first, and its
  /// destructor finishes a running loop while thread_pool_ still accepts the loop's tasks.
  ThreadPool submission_thread_{1};

  /// Start an import and return @p job (or a new job) without waiting for the placeholders: the
  /// placeholder loop runs on submission_thread_, metadata extraction on thread_pool_.
  /// job->on_progress_ reports the placeholder count while the loop runs, and job->on_finished_
  /// fires once every file is imported or failed.
  auto ImportToFolder(const std::vector<image_path_t>& paths, const image_path_t& dest,
                      const ImportOptions& options = {}, std::shared_ptr<ImportJob> job = nullptr)
      -> std::shared_ptr<ImportJob> override;

  void SyncImports(const ImportLogSnapshot& log_snapshot, const image_path_t& dest) override;

 private:
  void CreatePlaceholdersAndSubmit(const std::vector<image_path_t>& paths, const image_path_t& dest,
                                   const std::shared_ptr<ImportJob>&      job,
                                   const std::shared_ptr<ImportLog>&      import_log,
                                   const std::shared_ptr<ImportProgress>& progress);
};
};  // namespace alcedo
