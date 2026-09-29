//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "concurrency/thread_pool.hpp"
#include "pipeline_task.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
class PipelineScheduler {
 private:
  IncrID::IDGenerator<uint32_t> id_generator_{0};

  std::mutex                    scheduler_lock_;
  ThreadPool                    thread_pool_;  // use thred pool for now, can be changed to task scheduler later
  std::uint64_t                 next_request_id_{1};
  std::unordered_map<IFrameSink*, std::uint64_t> latest_submitted_request_id_;

  [[nodiscard]] bool IsStaleForSink(IFrameSink* sink, std::uint64_t request_id);
  void               MarkSinkApplyStarted(IFrameSink* sink, std::uint64_t request_id);

  /// Result that a finished task reports to PipelineTask::on_complete_.
  struct TaskOutcome {
    bool        success = false;
    std::string message;
  };

  /**
   * @brief Run @p task on the calling worker thread: prepare, load the input, then Apply the
   *        task's snapshot under the executor render lock and deliver the result.
   *
   * Sets the blocking result and runs the callbacks of @p task. Does not call on_complete_; the
   * caller calls it after this function returns, when the render lock is released.
   * @return The outcome for on_complete_. Never throws.
   */
  auto RunTask(PipelineTask& task) -> TaskOutcome;

 public:
  explicit PipelineScheduler();
  explicit PipelineScheduler(size_t thread_count);

  /**
   * @brief Schedule a pipeline task
   *
   * @param task
   */
  void ScheduleTask(PipelineTask&& task);

  /// Run arbitrary work on the same pool as pipeline tasks (test producers,
  /// async failure completion). Prefer ScheduleTask for real renders.
  void ScheduleWork(std::function<void()> work);

  /**
   * @brief Drop queued tasks that have not started and wait for running ones to finish.
   *
   * For an owner whose tasks capture state it is about to destroy. Dropped tasks run no callback
   * and no completion. Later ScheduleTask calls queue work that never runs.
   */
  void Shutdown();
};
};  // namespace alcedo