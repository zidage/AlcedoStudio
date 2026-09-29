//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
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

#include "app/batch_executor_pool.hpp"
#include "app/sleeve_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "image_pool_service.hpp"
#include "io/image/export_recipe.hpp"
#include "pipeline_service.hpp"
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

/**
 * @brief Renders export tasks from committed pipeline graph snapshots on executors it owns.
 *
 * Owner of one batch executor and one render worker, independent of the editor and the thumbnail
 * pool. Each task renders the committed snapshot captured when it was
 * queued, so an export shows the state the user committed before pressing Export, never an
 * uncommitted editor value and never a later edit.
 */
class ExportService {
 private:
  /// Render executors of this service. One: exports render one image at a time, and encoding
  /// runs in parallel on export_thread_pool_.
  static constexpr std::size_t kExportExecutorCount = 1;

  /**
   * @brief One queued export: the caller's task and the committed snapshot it renders.
   *
   * The snapshot is captured at enqueue so that the rendered pixels and the output color of the
   * recipe come from the same document state. It is immutable and released with the task.
   */
  struct QueuedExport {
    ExportTask                                   task_;
    std::shared_ptr<const PipelineGraphSnapshot> snapshot_;
  };

  std::shared_ptr<SleeveServiceImpl>   sleeve_service_;
  std::shared_ptr<ImagePoolService>    image_pool_service_;
  std::shared_ptr<PipelineMgmtService> pipeline_service_;

  // Declared before the scheduler: the scheduler joins its worker before the executors go.
  BatchExecutorPool                    executors_;
  PipelineScheduler                    render_scheduler_;

  std::list<QueuedExport>              export_queue_;
  std::mutex                           queue_mutex_;

  // We only use one mutex to protect the result collection
  // So we only support one export session involving multiple exports at a time
  std::mutex                           result_mutex_;

  // Keep this last so worker threads are joined before other members are torn down.
  ThreadPool                           export_thread_pool_{4};

  auto RunExportRenderTask(const QueuedExport& queued) -> ExportResult;

 public:
  ExportService() = delete;
  ExportService(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
                std::shared_ptr<ImagePoolService>    image_pool_service,
                std::shared_ptr<PipelineMgmtService> pipeline_service);

  ExportService(const ExportService&)            = delete;
  ExportService& operator=(const ExportService&) = delete;

  /** Build a sanitized name for preview or task planning without changing service state. */
  static auto    ResolveFileName(const ExportFileNameTemplate& name_template,
                                 const ExportFileNameContext& context, ImageFormatType format)
      -> ExportFileNameResult {
    return ResolveExportFileName(name_template, context, format);
  }

  /**
   * @brief Queue @p task with the committed snapshot of its image.
   *
   * Captures the committed pipeline graph snapshot of task.sleeve_id_ now
   * (PipelineMgmtService::AcquireCommittedSnapshot); the export renders exactly that state. When
   * the recipe has no explicit output color, it is read from the DRT node of the same snapshot, so
   * the pixels and the embedded ICC profile use one encoding.
   *
   * Thread: any thread; the snapshot may be built from storage on the calling thread.
   * @throws std::runtime_error when the recipe is missing, the image has no committed history, the
   *         committed document has no DRT node, or the resolved output color is invalid. Nothing is
   *         queued on failure.
   */
  void EnqueueExportTask(const ExportTask& task);

  void RemoveExportTask(sl_element_id_t sleeve_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    export_queue_.remove_if(
        [sleeve_id](const QueuedExport& queued) { return queued.task_.sleeve_id_ == sleeve_id; });
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
        [&id_set](const QueuedExport& queued) { return id_set.contains(queued.task_.sleeve_id_); });
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
