//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

#include "app/sleeve_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "image_pool_service.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"
#include "utils/import/import_error_code.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo {
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

  virtual void SyncImports(const ImportLogSnapshot& log_snapshot, const image_path_t& dest) = 0;
};

class ImportServiceImpl final : public ImportService {
 public:
  ImportServiceImpl() = delete;
  ImportServiceImpl(std::shared_ptr<SleeveServiceImpl> fs_service,
                    std::shared_ptr<ImagePoolService>  image_pool_service)
      : fs_service_(fs_service), image_pool_service_(image_pool_service) {}
  // ImportServiceImpl(std::shared_ptr<FileSystem>       fs,
  // std::shared_ptr<ImagePoolManager> image_pool_manager)
  // : fs_(fs), image_pool_manager_(image_pool_manager), fs_service_() {}

  ~ImportServiceImpl() = default;

  std::shared_ptr<SleeveServiceImpl>    fs_service_;

  std::shared_ptr<ImagePoolService> image_pool_service_ = nullptr;

  ThreadPool                            thread_pool_{8};

  auto ImportToFolder(const std::vector<image_path_t>& paths, const image_path_t& dest,
                      const ImportOptions& options = {}, std::shared_ptr<ImportJob> job = nullptr)
      -> std::shared_ptr<ImportJob> override;

  void SyncImports(const ImportLogSnapshot& log_snapshot, const image_path_t& dest) override;
};
};  // namespace alcedo
