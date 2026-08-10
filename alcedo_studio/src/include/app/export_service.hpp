//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/sleeve_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "image_pool_service.hpp"
#include "io/image/export_recipe.hpp"
#include "pipeline_service.hpp"
#include "render_service.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"

namespace alcedo {

struct ExportTask {
  sl_element_id_t                      sleeve_id_;
  image_id_t                           image_id_;
  ExportFormatOptions                  options_;
  std::optional<ExportRecipe>          recipe_;
  std::optional<ExportFileNameContext> file_name_context_;
};

struct ExportResult {
  bool                  success_                        = false;
  bool                  wrote_ultra_hdr_                = false;
  bool                  used_embedded_profile_fallback_ = false;
  bool                  metadata_written_               = false;
  bool                  icc_embedded_                   = false;
  bool                  resolution_tags_written_        = false;
  std::filesystem::path output_path_;
  std::string           failed_stage_;
  std::string           message_;
};

struct ExportProgress {
  size_t          total_         = 0;
  size_t          completed_     = 0;
  size_t          succeeded_     = 0;
  size_t          failed_        = 0;
  sl_element_id_t sleeve_id_     = 0;
  image_id_t      image_id_      = 0;
  bool            task_started_  = false;
  bool            task_finished_ = false;
  bool            task_success_  = false;
};

class ExportService {
 private:
  std::shared_ptr<SleeveServiceImpl>   sleeve_service_;
  std::shared_ptr<ImagePoolService>    image_pool_service_;
  std::shared_ptr<PipelineMgmtService> pipeline_service_;

  std::shared_ptr<PipelineScheduler>   pipeline_scheduler_;

  std::list<ExportTask>                export_queue_;
  std::mutex                           queue_mutex_;

  // We only use one mutex to protect the result collection
  // So we only support one export session involving multiple exports at a time
  std::mutex                           result_mutex_;

  // Keep this last so worker threads are joined before other members are torn down.
  ThreadPool                           export_thread_pool_{4};

  auto                                 RunExportRenderTask(const ExportTask& task) -> ExportResult;

 public:
  ExportService() = delete;
  ExportService(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
                std::shared_ptr<ImagePoolService>    image_pool_service,
                std::shared_ptr<PipelineMgmtService> pipeline_service)
      : sleeve_service_(std::move(sleeve_service)),
        image_pool_service_(std::move(image_pool_service)),
        pipeline_service_(std::move(pipeline_service)) {
    pipeline_scheduler_ = RenderService::GetThumbnailOrExportScheduler(1);
  };

  ExportService(const ExportService&)            = delete;
  ExportService& operator=(const ExportService&) = delete;

  /** Build a sanitized name for preview or task planning without changing service state. */
  static auto    ResolveFileName(const ExportFileNameTemplate& name_template,
                                 const ExportFileNameContext& context, ImageFormatType format)
      -> ExportFileNameResult {
    return ResolveExportFileName(name_template, context, format);
  }

  auto EnqueueExportTask(const ExportTask& task) -> void {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    export_queue_.emplace_back(task);
  }
  void RemoveExportTask(sl_element_id_t sleeve_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    export_queue_.remove_if(
        [sleeve_id](const ExportTask& task) { return task.sleeve_id_ == sleeve_id; });
  };
  void RemoveExportTasks(std::span<const sl_element_id_t> sleeve_ids) {
    std::lock_guard<std::mutex>         lock(queue_mutex_);
    std::unordered_set<sl_element_id_t> id_set;
    id_set.reserve(sleeve_ids.size() * 2 + 1);
    for (const auto sleeve_id : sleeve_ids) {
      if (sleeve_id != 0) {
        id_set.insert(sleeve_id);
      }
    }
    if (id_set.empty()) {
      return;
    }
    export_queue_.remove_if(
        [&id_set](const ExportTask& task) { return id_set.contains(task.sleeve_id_); });
  };
  void ClearAllExportTasks() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    export_queue_.clear();
  };

  void ExportAll(std::function<void(std::shared_ptr<std::vector<ExportResult>>)> callback);
  void ExportAll(std::function<void(const ExportProgress&)>                      progress_callback,
                 std::function<void(std::shared_ptr<std::vector<ExportResult>>)> callback);
};
};  // namespace alcedo
